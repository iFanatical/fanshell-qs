/* fanshell-qs-wallpaper-video <output> <mode> <file>: a video wallpaper on one
 * output, played by libmpv through its OpenGL render API, looped and silent,
 * decoded on the GPU where it can be (hwdec=auto-safe).
 *
 * A program of its own, per output, on a background-layer surface over the
 * shell's: a heavy video (4K at 60 fps) drawn on the shell's GUI thread held
 * up the bar, and libmpv loaded with fanshell-qs-shell slowed every start of
 * it. The shell (wallpaper.cpp) starts it and sends it lines on stdin --
 * pause, play, and "fade <ms> <mode> <path>" to cross-fade to the next
 * wallpaper and exit -- and it exits when stdin closes.
 *
 * Built when the shell is built for Wayland and libmpv is found; without it,
 * and on X11, a video wallpaper shows its first frame as a still image
 * (Wallpapers::still()). */
#include <cstdio>
#include <functional>
#include <memory>

#include <QApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSize>
#include <QSocketNotifier>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>
#include <QtGui/qguiapplication_platform.h>

#include <unistd.h>

#include "paths.h"
#include "surface.h"
#include "wallpaperimage.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <clocale>

namespace {

void *procAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(name)) : nullptr;
}

void set(mpv_handle *mpv, const char *name, const char *value)
{
    if (mpv_set_option_string(mpv, name, value) < 0)
        qWarning("fanshell-qs: mpv rejects %s=%s", name, value);
}

/* The exit status when wallpaper.video=gpu and mpv can only decode the video
 * on the CPU: the shell leaves the still showing. */
constexpr int cpuOnly = 3;

} /* namespace */

class VideoPlayer {
public:
    /* Starts `path`, laid out as `mode` (feh's modes, as for images), and
     * repaints `view` whenever a new frame is due. `view` must outlive the
     * player. */
    VideoPlayer(QWidget *view, const QString &path, const QString &mode);
    /* releaseGL() first, with the view's GL context current. */
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    const QString &path() const { return m_path; }
    const QString &mode() const { return m_mode; }

    /* Draws the current frame into framebuffer `fbo` of `size` device
     * pixels, with the view's GL context current. False, drawing nothing,
     * until the first frame has been decoded (or when the file cannot be
     * played), so the caller shows the still meanwhile. */
    bool render(int fbo, const QSize &size);

    void setPaused(bool paused);

    /* Called once, with the first frame: mpv's hwdec-current, the GPU
     * decoder it uses ("vaapi", ...), or empty when it decodes on the CPU. */
    std::function<void(const QString &)> onDecoder;

    /* Frees the render context; with the view's GL context current. */
    void releaseGL();

private:
    void drainEvents();

    QWidget *m_view;
    QString m_path;
    QString m_mode;
    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_gl = nullptr;
    bool m_frame = false;
    bool m_failed = false;
    bool m_paused = false;
    /* Reset on destruction; mpv's queued wakeups check it. */
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

VideoPlayer::VideoPlayer(QWidget *view, const QString &path, const QString &mode)
    : m_view(view), m_path(path), m_mode(mode)
{
    /* libmpv refuses to start unless numbers are formatted the C way, and
     * QCoreApplication set the locale from the environment. */
    std::setlocale(LC_NUMERIC, "C");
    m_mpv = mpv_create();
    if (!m_mpv) {
        m_failed = true;
        return;
    }
    set(m_mpv, "vo", "libmpv");
    set(m_mpv, "hwdec", "auto-safe");
    set(m_mpv, "audio", "no");
    set(m_mpv, "sid", "no");
    set(m_mpv, "loop-file", "inf");
    set(m_mpv, "keep-open", "yes");
    set(m_mpv, "config", "no");
    set(m_mpv, "terminal", "no");
    set(m_mpv, "osc", "no");
    set(m_mpv, "input-default-bindings", "no");
    set(m_mpv, "input-vo-keyboard", "no");
    set(m_mpv, "background-color", "#000000");
    /* feh's modes. tile has no counterpart; it fills instead. */
    if (mode == QLatin1String("center")) {
        set(m_mpv, "video-unscaled", "yes");
    } else if (mode == QLatin1String("scale")) {
        set(m_mpv, "keepaspect", "no");
    } else if (mode == QLatin1String("max")) {
        set(m_mpv, "panscan", "0");
    } else {
        set(m_mpv, "panscan", "1");
    }
    if (mpv_initialize(m_mpv) < 0) {
        qWarning("fanshell-qs: cannot start mpv for %s", qPrintable(path));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        m_failed = true;
        return;
    }
    /* Errors (a file mpv cannot play) arrive as events; read them on the
     * GUI thread. */
    mpv_set_wakeup_callback(
        m_mpv,
        [](void *self) {
            auto *p = static_cast<VideoPlayer *>(self);
            /* Queued: the player may be gone by the time it runs. */
            std::weak_ptr<bool> alive = p->m_alive;
            QMetaObject::invokeMethod(
                p->m_view,
                [p, alive]() {
                    if (alive.lock())
                        p->drainEvents();
                },
                Qt::QueuedConnection);
        },
        this);
    /* The file is loaded by render(), once the render context exists:
     * started before it, mpv finds no video output and gives up. */
}

VideoPlayer::~VideoPlayer()
{
    m_alive.reset();
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
    }
}

void VideoPlayer::drainEvents()
{
    if (!m_mpv)
        return;
    for (;;) {
        const mpv_event *e = mpv_wait_event(m_mpv, 0);
        if (e->event_id == MPV_EVENT_NONE)
            return;
        if (e->event_id == MPV_EVENT_END_FILE) {
            const auto *end = static_cast<const mpv_event_end_file *>(e->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR) {
                qWarning("fanshell-qs: cannot play wallpaper %s: %s",
                         qPrintable(m_path), mpv_error_string(end->error));
                m_failed = true;
                m_frame = false;
                m_view->update();
            }
        }
    }
}

bool VideoPlayer::render(int fbo, const QSize &size)
{
    if (m_failed || !m_mpv)
        return false;
    if (!m_gl) {
        mpv_opengl_init_params gl{procAddress, nullptr};
        int advanced = 1;
        QList<mpv_render_param> params{
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        };
        /* Lets VA-API hand decoded frames to GL without a copy. */
        if (auto *wl = qApp->nativeInterface<QNativeInterface::QWaylandApplication>())
            params.push_back({MPV_RENDER_PARAM_WL_DISPLAY, wl->display()});
        params.push_back({MPV_RENDER_PARAM_INVALID, nullptr});
        if (mpv_render_context_create(&m_gl, m_mpv, params.data()) < 0) {
            qWarning("fanshell-qs: cannot render %s with OpenGL", qPrintable(m_path));
            m_gl = nullptr;
            m_failed = true;
            return false;
        }
        mpv_render_context_set_update_callback(
            m_gl,
            [](void *view) {
                auto *w = static_cast<QWidget *>(view);
                QMetaObject::invokeMethod(w, [w]() { w->update(); },
                                          Qt::QueuedConnection);
            },
            m_view);
        const QByteArray file = m_path.toUtf8();
        const char *cmd[] = {"loadfile", file.constData(), nullptr};
        mpv_command_async(m_mpv, 0, cmd);
    }
    if ((mpv_render_context_update(m_gl) & MPV_RENDER_UPDATE_FRAME) && !m_frame) {
        m_frame = true;
        if (onDecoder) {
            char *hw = mpv_get_property_string(m_mpv, "hwdec-current");
            QString decoder = hw ? QString::fromUtf8(hw) : QString();
            mpv_free(hw);
            if (decoder == QLatin1String("no"))
                decoder.clear();
            /* Queued: it may end this process, not mid-render. */
            QMetaObject::invokeMethod(
                m_view, [cb = onDecoder, decoder]() { cb(decoder); }, Qt::QueuedConnection);
        }
    }
    if (!m_frame)
        return false;
    mpv_opengl_fbo target{fbo, size.width(), size.height(), 0};
    int flip = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_gl, params);
    return true;
}

void VideoPlayer::setPaused(bool paused)
{
    if (!m_mpv || paused == m_paused)
        return;
    m_paused = paused;
    mpv_set_property_string(m_mpv, "pause", paused ? "yes" : "no");
}

void VideoPlayer::releaseGL()
{
    if (m_gl) {
        mpv_render_context_free(m_gl);
        m_gl = nullptr;
    }
}


/* ------------------------------------------------- the video's own process */

namespace {

/* One output's video, on the background layer above the shell's own
 * wallpaper surface (made later, so stacked over it), which shows the
 * video's first frame underneath. */
class VideoSurface : public QOpenGLWidget {
public:
    VideoSurface(QScreen *screen, const QString &path, const QString &mode)
        : QOpenGLWidget(nullptr)
    {
        setWindowTitle(QStringLiteral("fanshell-qs-wallpaper-video"));
        Surface::SurfaceSpec spec;
        spec.layer = Surface::Layer::Background;
        spec.anchors = Surface::AnchorTop | Surface::AnchorBottom
                       | Surface::AnchorLeft | Surface::AnchorRight;
        spec.exclusiveZone = -1;
        spec.keyboard = Surface::Keyboard::None;
        spec.scope = QStringLiteral("fanshell-qs-wallpaper-video");
        resize(screen->geometry().size());
        Surface::configure(this, screen, spec);
        m_px = (QSizeF(screen->geometry().size()) * screen->devicePixelRatio()).toSize();
        /* Shown until mpv's first frame: what the shell shows beneath. */
        m_poster = Wallpapers::render(path, mode, m_px);
        m_player.reset(new VideoPlayer(this, path, mode));
        /* wallpaper.video=gpu: a video the GPU cannot decode (AV1 on older
         * Intel graphics, say) costs several cores; leave the still beneath
         * instead. */
        if (Paths::setting(QStringLiteral("wallpaper.video")).toLower()
                == QLatin1String("gpu"))
            m_player->onDecoder = [path](const QString &decoder) {
                if (!decoder.isEmpty())
                    return;
                fprintf(stderr, "fanshell-qs: %s decodes on the CPU only; showing "
                                "its still (wallpaper.video=gpu)\n",
                        qPrintable(path));
                QCoreApplication::exit(cpuOnly);
            };

        m_anim.setStartValue(0.0);
        m_anim.setEndValue(1.0);
        m_anim.setEasingCurve(QEasingCurve::InOutQuad);
        QObject::connect(&m_anim, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &v) {
                             m_t = v.toReal();
                             update();
                         });
        /* Covered by the wallpaper that replaces it, as the shell's surface
         * beneath shows it: going now changes nothing on screen. */
        QObject::connect(&m_anim, &QVariantAnimation::finished, qApp,
                         &QCoreApplication::quit, Qt::QueuedConnection);
    }

    ~VideoSurface() override
    {
        makeCurrent();
        m_player->releaseGL();
        doneCurrent();
    }

    /* A line from the shell: pause, play, or "fade <ms> <mode> <path>",
     * to fade to that wallpaper and go. */
    void command(const QString &line)
    {
        if (line == QLatin1String("pause") || line == QLatin1String("play")) {
            m_player->setPaused(line == QLatin1String("pause"));
            return;
        }
        if (!line.startsWith(QLatin1String("fade ")) || m_fading)
            return;
        m_fading = true;
        const QStringList f = line.split(QLatin1Char(' '));
        const int ms = f.value(1).toInt();
        const QString mode = f.value(2);
        const QString path = line.section(QLatin1Char(' '), 3);
        if (ms <= 0 || path.isEmpty()) {
            QCoreApplication::quit();
            return;
        }
        /* Decoded aside, so the video does not stall meanwhile. */
        auto image = std::make_shared<QImage>();
        const QSize px = m_px;
        QThread *thread = QThread::create(
            [image, path, mode, px]() { *image = Wallpapers::render(path, mode, px); });
        QObject::connect(thread, &QThread::finished, this, [this, thread, image, ms]() {
            thread->deleteLater();
            if (image->isNull()) {
                QCoreApplication::quit();
                return;
            }
            m_next = *image;
            m_anim.setDuration(ms);
            m_anim.start();
        });
        thread->start();
    }

protected:
    void paintGL() override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.beginNativePainting();
        const bool video = m_player->render(int(defaultFramebufferObject()), m_px);
        p.endNativePainting();
        if (!video) {
            p.fillRect(rect(), Qt::black);
            if (!m_poster.isNull())
                p.drawImage(rect(), m_poster);
        }
        if (!m_next.isNull()) {
            p.setOpacity(m_t);
            p.drawImage(rect(), m_next);
        }
    }

private:
    QSize m_px;
    QImage m_poster;
    QImage m_next;
    qreal m_t = 0.0;
    bool m_fading = false;
    QVariantAnimation m_anim;
    std::unique_ptr<VideoPlayer> m_player;
};

} /* namespace */

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: fanshell-qs-wallpaper-video <output> <mode> <file>\n");
        return 2;
    }
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fanshell-qs-wallpaper-video"));
    app.setQuitOnLastWindowClosed(false);
    if (!Surface::wayland()) {
        fprintf(stderr, "fanshell-qs-wallpaper-video: Wayland only\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    const QString mode = QString::fromLocal8Bit(argv[2]);
    const QString path = QString::fromLocal8Bit(argv[3]);
    QScreen *screen = nullptr;
    for (QScreen *s : QGuiApplication::screens())
        if (s->name() == output)
            screen = s;
    if (!screen)
        return 1;
    VideoSurface surface(screen, path, mode);
    QObject::connect(qApp, &QGuiApplication::screenRemoved, qApp, [screen](QScreen *s) {
        if (s == screen)
            QCoreApplication::quit();
    });

    /* Commands from the shell on stdin, a line each; its end (the shell
     * gone) ends this too. */
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QByteArray pending;
    QObject::connect(&input, &QSocketNotifier::activated, qApp, [&]() {
        char buf[4096];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) {
            input.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        pending.append(buf, int(n));
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            surface.command(QString::fromUtf8(pending.left(nl)));
            pending.remove(0, nl + 1);
        }
    });

    surface.show();
    return app.exec();
}
