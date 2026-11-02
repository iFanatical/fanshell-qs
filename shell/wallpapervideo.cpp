#include "wallpapervideo.h"

#include "surface.h"

#ifdef FANSHELL_QS_HAVE_MPV

#include <QGuiApplication>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QPointer>
#include <QWidget>
#include <QtGui/qguiapplication_platform.h>

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

} /* namespace */

bool VideoPlayer::available()
{
    return Surface::wayland();
}

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
    if (mpv_render_context_update(m_gl) & MPV_RENDER_UPDATE_FRAME)
        m_frame = true;
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

#else /* !FANSHELL_QS_HAVE_MPV */

bool VideoPlayer::available()
{
    return false;
}

VideoPlayer::VideoPlayer(QWidget *view, const QString &path, const QString &mode)
    : m_view(view), m_path(path), m_mode(mode), m_failed(true)
{
}

VideoPlayer::~VideoPlayer() = default;

void VideoPlayer::drainEvents() {}

bool VideoPlayer::render(int, const QSize &)
{
    return false;
}

void VideoPlayer::setPaused(bool) {}

void VideoPlayer::releaseGL() {}

#endif
