#include "wallpaper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QThread>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <xcb/xcb.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "overlay.h"
#include "paths.h"
#include "popup.h"
#include "surface.h"
#include "theme.h"
#include "widgets.h"
#include "wmstate.h"

namespace {

constexpr auto stateName = "wallpaper.conf";

const QStringList modes{QStringLiteral("fill"), QStringLiteral("center"),
                        QStringLiteral("max"), QStringLiteral("scale"),
                        QStringLiteral("tile")};

QString validMode(const QString &mode, const QString &fallback)
{
    const QString m = mode.toLower();
    return modes.contains(m) ? m : fallback;
}

/* The path the last feh-written ~/.fehbg shows, for a one-time import. It is
 * a shell script whose last word is the image:
 *     feh --no-fehbg --bg-fill '/path/to/file.jpg' */
QString fehbgPath()
{
    QFile file(QDir::homePath() + QStringLiteral("/.fehbg"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QString path;
    for (const QString &raw : QString::fromUtf8(file.readAll()).split('\n')) {
        const QString line = raw.trimmed();
        if (!line.contains(QLatin1String("feh")) || !line.endsWith('\''))
            continue;
        const int open = line.lastIndexOf('\'', line.size() - 2);
        if (open >= 0)
            path = line.mid(open + 1, line.size() - open - 2);
    }
    return path;
}

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

xcb_atom_t atom(xcb_connection_t *c, const char *name, bool create)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, !create, uint16_t(strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

xcb_pixmap_t pixmapProperty(xcb_connection_t *c, xcb_window_t root, xcb_atom_t prop)
{
    xcb_pixmap_t pixmap = XCB_NONE;
    if (prop == XCB_ATOM_NONE)
        return pixmap;
    if (xcb_get_property_reply_t *p = xcb_get_property_reply(
            c, xcb_get_property(c, 0, root, prop, XCB_ATOM_PIXMAP, 0, 1), nullptr)) {
        if (p->type == XCB_ATOM_PIXMAP && p->format == 32
                && xcb_get_property_value_length(p) == 4)
            pixmap = *static_cast<const xcb_pixmap_t *>(xcb_get_property_value(p));
        free(p);
    }
    return pixmap;
}

/* Whether images of `depth` are 32 bits a pixel, little-endian: what a
 * Format_RGB32 QImage holds, byte for byte. */
bool rgb32Server(xcb_connection_t *c, uint8_t depth)
{
    const xcb_setup_t *setup = xcb_get_setup(c);
    if (setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST)
        return false;
    for (auto it = xcb_setup_pixmap_formats_iterator(setup); it.rem;
         xcb_format_next(&it))
        if (it.data->depth == depth)
            return it.data->bits_per_pixel == 32;
    return false;
}

QSize rootSize(xcb_connection_t *c, xcb_window_t root)
{
    QSize size;
    if (xcb_get_geometry_reply_t *g =
            xcb_get_geometry_reply(c, xcb_get_geometry(c, root), nullptr)) {
        size = QSize(g->width, g->height);
        free(g);
    }
    return size;
}

/* The root window's background as wallpaper setters publish it
 * (_XROOTPMAP_ID), or a null image when there is none or it is not 24-bit
 * RGB32. */
QImage rootBackground()
{
    xcb_connection_t *c = conn();
    if (!c)
        return QImage();
    const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    const xcb_pixmap_t pixmap =
        pixmapProperty(c, root, atom(c, "_XROOTPMAP_ID", false));
    if (pixmap == XCB_NONE || !rgb32Server(c, 24))
        return QImage();
    QSize size;
    if (xcb_get_geometry_reply_t *g =
            xcb_get_geometry_reply(c, xcb_get_geometry(c, pixmap), nullptr)) {
        size = QSize(g->width, g->height);
        free(g);
    }
    if (size.isEmpty())
        return QImage();
    const int w = size.width(), h = size.height();
    xcb_get_image_reply_t *img = xcb_get_image_reply(
        c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0, uint16_t(w),
                         uint16_t(h), ~0u),
        nullptr);
    if (!img)
        return QImage();
    QImage out;
    if (img->depth == 24 && xcb_get_image_data_length(img) == w * h * 4) {
        out = QImage(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; y++)
            memcpy(out.scanLine(y), xcb_get_image_data(img) + y * w * 4, size_t(w) * 4);
    }
    free(img);
    return out;
}

/* Make `image` the root window's background, as feh and hsetroot do: a
 * pixmap made on a connection of its own, kept after it closes
 * (RetainPermanent) and published in _XROOTPMAP_ID and ESETROOT_PMAP_ID.
 * The previous one, when a setter like this left it, is freed by killing
 * its (closed) client. */
bool setRootPixmap(const QImage &image)
{
    xcb_connection_t *c = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(c)) {
        xcb_disconnect(c);
        return false;
    }
    const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const xcb_window_t root = screen->root;
    const QImage img = image.convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    if (screen->root_depth != 24 || !rgb32Server(c, 24) || w <= 0 || h <= 0
            || w > 65535 || h > 65535) {
        xcb_disconnect(c);
        return false;
    }

    const xcb_pixmap_t pixmap = xcb_generate_id(c);
    xcb_create_pixmap(c, screen->root_depth, pixmap, root, uint16_t(w), uint16_t(h));
    const xcb_gcontext_t gc = xcb_generate_id(c);
    xcb_create_gc(c, gc, pixmap, 0, nullptr);
    /* In bands that fit one request. */
    const uint32_t maxBytes = xcb_get_maximum_request_length(c) * 4 - 64;
    const int band = qMax(1, int(maxBytes / uint32_t(w * 4)));
    for (int y = 0; y < h; y += band) {
        const int rows = qMin(band, h - y);
        xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, uint16_t(w),
                      uint16_t(rows), 0, int16_t(y), 0, screen->root_depth,
                      uint32_t(rows * w * 4), img.constScanLine(y));
    }
    xcb_free_gc(c, gc);

    const xcb_atom_t rootPmap = atom(c, "_XROOTPMAP_ID", true);
    const xcb_atom_t esetroot = atom(c, "ESETROOT_PMAP_ID", true);
    const xcb_pixmap_t old = pixmapProperty(c, root, rootPmap);
    const bool oldIsOurs = old != XCB_NONE && old == pixmapProperty(c, root, esetroot);

    xcb_change_window_attributes(c, root, XCB_CW_BACK_PIXMAP, &pixmap);
    xcb_clear_area(c, 0, root, 0, 0, 0, 0);
    for (xcb_atom_t prop : {rootPmap, esetroot})
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, prop, XCB_ATOM_PIXMAP,
                            32, 1, &pixmap);
    if (oldIsOurs)
        xcb_kill_client(c, old);
    xcb_set_close_down_mode(c, XCB_CLOSE_DOWN_RETAIN_PERMANENT);
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr));
    xcb_disconnect(c);
    return true;
}

struct Head {
    QString name;
    QRect rect;
};

/* One output's part of the fade: an override-redirect window at the bottom
 * of the stack, above the root background and below every other window.
 * It has an alpha channel so picom never takes it for an opaque fullscreen
 * window and stops compositing (unredir-if-fullscreen). */
class FadeWindow : public QWidget {
public:
    FadeWindow(const QRect &geometry, const QImage &from)
        : QWidget(nullptr, Qt::X11BypassWindowManagerHint | Qt::FramelessWindowHint
                               | Qt::WindowDoesNotAcceptFocus
                               | Qt::WindowTransparentForInput),
          m_from(from)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_X11DoNotAcceptFocus);
        setWindowTitle(QStringLiteral("fanshell-qs-wallpaper-fade"));
        setGeometry(geometry);
    }

    std::function<void()> painted;

    void setTo(const QImage &to) { m_to = to; }
    void setProgress(qreal t)
    {
        m_t = t;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(0, 0, m_from);
        if (!m_to.isNull() && m_t > 0) {
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setOpacity(m_t);
            p.drawImage(0, 0, m_to);
        }
        if (painted) {
            auto done = std::move(painted);
            painted = nullptr;
            done();
        }
    }

private:
    QImage m_from;
    QImage m_to;
    qreal m_t = 0;
};


} /* namespace */

/* One output's wallpaper on Wayland: a background-layer surface covering
 * it, beneath every window. A new image fades in over the old one. */
class WallpaperLayer : public QWidget {
public:
    explicit WallpaperLayer(QScreen *screen) : QWidget(nullptr)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setWindowTitle(QStringLiteral("fanshell-qs-wallpaper"));
        Surface::SurfaceSpec spec;
        spec.layer = Surface::Layer::Background;
        spec.anchors = Surface::AnchorTop | Surface::AnchorBottom
                       | Surface::AnchorLeft | Surface::AnchorRight;
        spec.exclusiveZone = -1;
        spec.keyboard = Surface::Keyboard::None;
        spec.scope = QStringLiteral("fanshell-qs-wallpaper");
        resize(screen->geometry().size());
        Surface::configure(this, screen, spec);
        m_anim.setStartValue(0.0);
        m_anim.setEndValue(1.0);
        m_anim.setEasingCurve(QEasingCurve::InOutQuad);
        QObject::connect(&m_anim, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &v) {
                             m_t = v.toReal();
                             update();
                         });
        QObject::connect(&m_anim, &QVariantAnimation::finished, this, [this]() {
            m_from = QImage();
            update();
        });
    }

    void setImage(const QImage &image, int fadeMs)
    {
        m_anim.stop();
        m_from = fadeMs > 0 && isVisible() ? m_to : QImage();
        m_to = image;
        m_t = m_from.isNull() ? 1.0 : 0.0;
        if (!isVisible())
            show();
        if (!m_from.isNull()) {
            m_anim.setDuration(fadeMs);
            m_anim.start();
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.fillRect(rect(), Qt::black);
        /* Rendered at device pixels; drawn into the logical rect. */
        if (!m_from.isNull())
            p.drawImage(rect(), m_from);
        if (!m_to.isNull()) {
            p.setOpacity(m_t);
            p.drawImage(rect(), m_to);
        }
    }

private:
    QImage m_from;
    QImage m_to;
    qreal m_t = 1.0;
    QVariantAnimation m_anim;
};

namespace {
} /* namespace */

/* ------------------------------------------------------------- Wallpapers */

Wallpapers::Choice Wallpapers::Config::forOutput(const QString &output) const
{
    Choice c = outputs.value(output, all);
    if (c.path.isEmpty())
        c.path = all.path;
    c.mode = validMode(c.mode, validMode(all.mode, QStringLiteral("fill")));
    return c;
}

Wallpapers::Config Wallpapers::Config::load()
{
    Config conf;
    const QHash<QString, QString> kv = Paths::readConf(QLatin1String(stateName));
    conf.all = {kv.value(QStringLiteral("path")), kv.value(QStringLiteral("mode"))};
    for (auto it = kv.constBegin(); it != kv.constEnd(); ++it) {
        if (it.key().startsWith(QLatin1String("path.")))
            conf.outputs[it.key().mid(5)].path = it.value();
        else if (it.key().startsWith(QLatin1String("mode.")))
            conf.outputs[it.key().mid(5)].mode = it.value();
    }
    return conf;
}

void Wallpapers::Config::save() const
{
    QStringList lines{QStringLiteral("path=") + all.path,
                      QStringLiteral("mode=") + all.mode};
    QStringList names = outputs.keys();
    names.sort();
    for (const QString &name : std::as_const(names)) {
        const Choice &c = outputs[name];
        lines << QStringLiteral("path.%1=%2").arg(name, c.path)
              << QStringLiteral("mode.%1=%2").arg(name, c.mode);
    }
    if (!Paths::writeConf(QLatin1String(stateName),
                          QStringLiteral("Wallpapers set from the picker; the shell "
                                         "draws them at startup.\npath/mode: every "
                                         "output; path.<output>/mode.<output>: one "
                                         "that differs."),
                          lines))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(stateName))));
}

QImage Wallpapers::render(const QString &path, const QString &mode, const QSize &size)
{
    if (path.isEmpty() || size.isEmpty())
        return QImage();
    QImageReader reader(path);
    reader.setAutoTransform(true);
    /* Have the decoder shrink what will be shrunk anyway; much cheaper for
     * large JPEGs than decoding at full size. */
    const QSize raw = reader.size();
    if (raw.isValid() && (mode == QLatin1String("fill") || mode == QLatin1String("max")
                          || mode == QLatin1String("scale"))) {
        QSize shown = raw;
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            shown.transpose();
        const double fx = double(size.width()) / shown.width();
        const double fy = double(size.height()) / shown.height();
        const double f = mode == QLatin1String("max") ? qMin(fx, fy) : qMax(fx, fy);
        if (f < 1.0)
            reader.setScaledSize(QSize(qMax(1, int(std::ceil(raw.width() * f))),
                                       qMax(1, int(std::ceil(raw.height() * f)))));
    }
    const QImage img = reader.read();
    if (img.isNull())
        return QImage();

    QImage out(size, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    auto centred = [&p, &size](const QImage &i) {
        p.drawImage((size.width() - i.width()) / 2, (size.height() - i.height()) / 2, i);
    };
    if (mode == QLatin1String("center"))
        centred(img);
    else if (mode == QLatin1String("tile"))
        p.fillRect(out.rect(), QBrush(img));
    else if (mode == QLatin1String("scale"))
        p.drawImage(0, 0, img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    else if (mode == QLatin1String("max"))
        centred(img.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        centred(img.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    p.end();
    return out;
}

/* ------------------------------------------------------------ WallpaperFade */

/* Covers the outputs with the old wallpaper while the new one is set, then
 * cross-fades to it. See wallpaper.h. */
class WallpaperFade : public QObject {
public:
    explicit WallpaperFade(QObject *parent) : QObject(parent)
    {
        m_anim.setStartValue(0.0);
        m_anim.setEndValue(1.0);
        m_anim.setEasingCurve(QEasingCurve::InOutQuad);
        connect(&m_anim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) {
                    for (FadeWindow *w : std::as_const(m_windows))
                        w->setProgress(v.toReal());
                });
        connect(&m_anim, &QVariantAnimation::finished, this, [this]() { clear(); });
    }

    /* wallpaper.fade_ms, or 0 when wallpaper.fade=off. */
    static int duration()
    {
        if (Paths::setting(QStringLiteral("wallpaper.fade")).toLower()
                == QLatin1String("off"))
            return 0;
        bool ok = false;
        const int ms = Paths::setting(QStringLiteral("wallpaper.fade_ms")).toInt(&ok);
        return ok ? std::clamp(ms, 0, 10000) : 500;
    }

    bool busy() const { return !m_windows.isEmpty(); }

    /* Show the old wallpaper over every output and call `covered` once it
     * is on screen. False, without calling it, when there is nothing to
     * fade from. */
    bool cover(std::function<void()> covered)
    {
        if (busy())
            return false;
        const QImage root = rootBackground();
        if (root.isNull())
            return false;
        /* Once, when every window has painted -- or after half a second
         * regardless, so a window that is never exposed cannot hold it up. */
        auto once = std::make_shared<std::function<void()>>(std::move(covered));
        auto fire = [once]() {
            if (*once) {
                auto f = std::move(*once);
                *once = nullptr;
                f();
            }
        };
        QTimer::singleShot(500, this, fire);
        const QList<QScreen *> screens = QGuiApplication::screens();
        auto pending = std::make_shared<int>(0);
        for (QScreen *s : screens) {
            const QRect g = s->geometry() & root.rect();
            if (g.isEmpty())
                continue;
            auto *w = new FadeWindow(g, root.copy(g));
            ++*pending;
            /* A frame for picom to show it too, before the root changes. */
            w->painted = [this, pending, fire]() {
                if (--*pending == 0)
                    QTimer::singleShot(30, this, fire);
            };
            m_windows << w;
            w->show();
            w->lower();
        }
        if (m_windows.isEmpty())
            *once = nullptr;
        return !m_windows.isEmpty();
    }

    /* The new wallpaper is set: fade to `root`, or just uncover. */
    void reveal(const QImage &root, int ms)
    {
        if (root.isNull()) {
            clear();
            return;
        }
        for (FadeWindow *w : std::as_const(m_windows))
            w->setTo(root.copy(w->geometry()));
        m_anim.setDuration(qMax(1, ms));
        m_anim.start();
    }

private:
    void clear()
    {
        m_anim.stop();
        for (FadeWindow *w : std::as_const(m_windows)) {
            w->hide();
            w->deleteLater();
        }
        m_windows.clear();
    }

    QVector<FadeWindow *> m_windows;
    QVariantAnimation m_anim;
};

/* ---------------------------------------------------------- WallpaperPicker */

WallpaperPicker *WallpaperPicker::instance()
{
    static WallpaperPicker *s = new WallpaperPicker();
    return s;
}

QString WallpaperPicker::wallpaperDir()
{
    return Paths::setting(QStringLiteral("wallpaper.dir"),
                          QDir::homePath() + QStringLiteral("/Pictures/wallpapers"));
}

QString WallpaperPicker::mode()
{
    return validMode(Paths::setting(QStringLiteral("wallpaper.mode")),
                     QStringLiteral("fill"));
}

QStringList WallpaperPicker::outputNames()
{
    QStringList names;
    for (QScreen *s : QGuiApplication::screens())
        names << s->name();
    names.sort();
    return names;
}

WallpaperPicker::WallpaperPicker(QObject *parent) : QObject(parent)
{
    m_win = new WallpaperWindow(this);
    m_fade = new WallpaperFade(this);

    m_decoder.setSingleShot(false);
    m_decoder.setInterval(0);
    connect(&m_decoder, &QTimer::timeout, this, &WallpaperPicker::decodeNext);

    m_outputsChanged.setSingleShot(true);
    m_outputsChanged.setInterval(1000);
    connect(&m_outputsChanged, &QTimer::timeout, this, &WallpaperPicker::restore);
    auto watch = [this](QScreen *s) {
        connect(s, &QScreen::geometryChanged, &m_outputsChanged,
                qOverload<>(&QTimer::start));
    };
    for (QScreen *s : QGuiApplication::screens())
        watch(s);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, watch](QScreen *s) {
        watch(s);
        m_outputsChanged.start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, &m_outputsChanged,
            qOverload<>(&QTimer::start));

    m_conf = Wallpapers::Config::load();
    if (m_conf.all.path.isEmpty() && m_conf.outputs.isEmpty()) {
        /* First run since the state moved out of ~/.fehbg: carry it over. */
        m_conf.all = {fehbgPath(), mode()};
        if (!m_conf.all.path.isEmpty())
            m_conf.save();
    }

    rescan();
    syncSelectionToCurrent();
    restore();
}

void WallpaperPicker::rescan()
{
    /* Deliberately not recursive: see wallpaper.h. */
    QDir dir(wallpaperDir());
    const QStringList globs{
        QStringLiteral("*.png"),  QStringLiteral("*.jpg"),
        QStringLiteral("*.jpeg"), QStringLiteral("*.webp"),
        QStringLiteral("*.gif"),  QStringLiteral("*.bmp")};
    const QFileInfoList files =
        dir.entryInfoList(globs, QDir::Files | QDir::Readable, QDir::Name);

    /* Keep already-decoded thumbnails across a rescan. */
    QHash<QString, QPixmap> keep;
    for (const Wallpaper &w : std::as_const(entries))
        if (w.decoded && !w.thumb.isNull())
            keep.insert(w.path, w.thumb);

    entries.clear();
    for (const QFileInfo &fi : files) {
        Wallpaper w;
        w.path = fi.absoluteFilePath();
        w.name = fi.completeBaseName();
        const auto it = keep.constFind(w.path);
        if (it != keep.constEnd()) {
            w.thumb = *it;
            w.decoded = true;
        }
        entries.push_back(w);
    }
    if (selected >= entries.size())
        selected = qMax(0, entries.size() - 1);

    bool pending = false;
    for (const Wallpaper &w : std::as_const(entries))
        pending = pending || !w.decoded;
    if (pending)
        m_decoder.start();

    emit changed();
}

void WallpaperPicker::decodeNext()
{
    for (Wallpaper &w : entries) {
        if (w.decoded)
            continue;
        QImageReader r(w.path);
        r.setAutoTransform(true);
        /* Ask the decoder itself to downscale -- libjpeg/libpng can do this
         * while reading, which is far cheaper than decoding full size and
         * scaling after (12MB PNGs here). */
        QSize s = r.size();
        if (s.isValid()) {
            s.scale(thumbW, thumbH, Qt::KeepAspectRatioByExpanding);
            r.setScaledSize(s);
        }
        const QImage img = r.read();
        if (!img.isNull()) {
            /* Cover-crop to the cell so the grid stays a tidy lattice
             * regardless of each wallpaper's aspect ratio. */
            const QImage scaled = img.scaled(thumbW, thumbH,
                                             Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation);
            const int dx = (scaled.width() - thumbW) / 2;
            const int dy = (scaled.height() - thumbH) / 2;
            w.thumb = QPixmap::fromImage(
                scaled.copy(dx, dy, thumbW, thumbH));
        }
        w.decoded = true;
        emit changed();
        return;
    }
    m_decoder.stop();
}

bool WallpaperPicker::isCurrent(const QString &path) const
{
    if (!target.isEmpty())
        return m_conf.forOutput(target).path == path;
    for (const QString &name : outputNames())
        if (m_conf.forOutput(name).path == path)
            return true;
    return false;
}

void WallpaperPicker::drawLayers(const Wallpapers::Config &conf, bool fade,
                                 bool save)
{
    struct Out {
        QScreen *screen;
        QString name;
        QSize size; /* device pixels */
    };
    QVector<Out> outs;
    for (QScreen *s : QGuiApplication::screens())
        outs.push_back({s, s->name(),
                        (QSizeF(s->geometry().size()) * s->devicePixelRatio())
                            .toSize()});
    /* Outputs that went away take their surface with them. */
    for (auto it = m_layers.begin(); it != m_layers.end();) {
        const bool gone = std::none_of(outs.begin(), outs.end(),
                                       [&it](const Out &o) { return o.screen == it.key(); });
        if (gone) {
            it.value()->deleteLater();
            it = m_layers.erase(it);
        } else {
            ++it;
        }
    }
    if (outs.isEmpty())
        return;

    const int generation = ++m_generation;
    auto images = std::make_shared<QHash<QString, QImage>>();
    auto failed = std::make_shared<QString>();
    QThread *thread = QThread::create([conf, outs, images, failed]() {
        for (const Out &o : outs) {
            const Wallpapers::Choice choice = conf.forOutput(o.name);
            if (choice.path.isEmpty())
                continue;
            const QImage img = Wallpapers::render(choice.path, choice.mode, o.size);
            if (img.isNull()) {
                *failed = choice.path;
                return;
            }
            images->insert(o.name, img);
        }
    });
    connect(thread, &QThread::finished, this,
            [this, thread, conf, fade, save, generation, images, failed, outs]() {
        thread->deleteLater();
        if (generation != m_generation)
            return;
        if (!failed->isEmpty()) {
            qWarning("fanshell-qs: cannot read wallpaper %s", qPrintable(*failed));
            return;
        }
        m_conf = conf;
        if (save)
            m_conf.save();
        emit changed();
        const int ms = fade ? WallpaperFade::duration() : 0;
        for (const Out &o : outs) {
            if (!QGuiApplication::screens().contains(o.screen))
                continue;
            WallpaperLayer *&layer = m_layers[o.screen];
            if (!layer)
                layer = new WallpaperLayer(o.screen);
            layer->setImage(images->value(o.name), ms);
        }
    });
    thread->start();
}

void WallpaperPicker::draw(const Wallpapers::Config &conf, bool fade, bool save)
{
    if (Surface::wayland()) {
        drawLayers(conf, fade, save);
        return;
    }
    xcb_connection_t *c = conn();
    if (!c)
        return;
    const QSize size =
        rootSize(c, xcb_setup_roots_iterator(xcb_get_setup(c)).data->root);
    QVector<Head> heads;
    for (QScreen *s : QGuiApplication::screens())
        heads.push_back({s->name(), s->geometry()});
    if (size.isEmpty() || heads.isEmpty())
        return;

    /* Decoding a large image takes a few hundred ms: off the GUI thread,
     * so the bar does not freeze meanwhile. */
    const int generation = ++m_generation;
    auto root = std::make_shared<QImage>();
    auto failed = std::make_shared<QString>();
    QThread *thread = QThread::create([conf, heads, size, root, failed]() {
        QImage out(size, QImage::Format_RGB32);
        out.fill(Qt::black);
        QPainter p(&out);
        QHash<QString, QImage> done;
        for (const Head &h : heads) {
            const Wallpapers::Choice choice = conf.forOutput(h.name);
            if (choice.path.isEmpty())
                continue;
            const QString key = QStringLiteral("%1\n%2\n%3x%4").arg(
                choice.path, choice.mode).arg(h.rect.width()).arg(h.rect.height());
            auto it = done.find(key);
            if (it == done.end())
                it = done.insert(key, Wallpapers::render(choice.path, choice.mode,
                                                         h.rect.size()));
            if (it->isNull()) {
                *failed = choice.path;
                return;
            }
            p.drawImage(h.rect.topLeft(), *it);
        }
        p.end();
        *root = out;
    });
    connect(thread, &QThread::finished, this,
            [this, thread, conf, fade, save, generation, root, failed]() {
        thread->deleteLater();
        if (generation != m_generation)
            return;
        if (!failed->isEmpty()) {
            qWarning("fanshell-qs: cannot read wallpaper %s", qPrintable(*failed));
            return;
        }
        if (root->isNull())
            return;
        m_conf = conf;
        if (save)
            m_conf.save();
        emit changed();
        const QImage image = *root;
        const int ms = fade ? WallpaperFade::duration() : 0;
        const bool fading = ms > 0 && m_fade->cover([this, image, ms]() {
            const bool ok = setRootPixmap(image);
            m_fade->reveal(ok ? image : QImage(), ms);
        });
        if (!fading && !setRootPixmap(image))
            qWarning("fanshell-qs: cannot set the root window's background");
    });
    thread->start();
}

void WallpaperPicker::apply(int index)
{
    if (index < 0 || index >= entries.size())
        return;
    Wallpapers::Config conf = m_conf;
    const Wallpapers::Choice choice{entries[index].path, mode()};
    if (target.isEmpty()) {
        conf.all = choice;
        conf.outputs.clear();
    } else {
        conf.outputs[target] = choice;
    }
    draw(conf, true, true);
}

void WallpaperPicker::restore()
{
    /* Each with the mode it was set with, so a later wallpaper.mode change
     * does not silently re-crop what is already chosen. */
    draw(m_conf, false, false);
}

void WallpaperPicker::syncSelectionToCurrent()
{
    for (int i = 0; i < entries.size(); i++)
        if (isCurrent(entries[i].path)) {
            selected = i;
            return;
        }
}

void WallpaperPicker::move(int delta)
{
    if (entries.isEmpty())
        return;
    selected = qBound(0, selected + delta, entries.size() - 1);
    emit selectionChanged();
}

void WallpaperPicker::openPicker()
{
    /* Pick up anything added to the directory since the last open, and
     * re-read wallpaper.conf. */
    rescan();
    m_conf = Wallpapers::Config::load();
    if (!target.isEmpty() && !outputNames().contains(target))
        target.clear();
    syncSelectionToCurrent();
    /* openOn() resolves the focused output itself. */
    m_win->openOn(nullptr);
}

void WallpaperPicker::togglePicker()
{
    if (m_win->isVisible())
        hidePicker();
    else
        openPicker();
}

void WallpaperPicker::hidePicker()
{
    m_win->hide();
}

/* ------------------------------------------------------------ WallpaperGrid */

WallpaperGrid::WallpaperGrid(WallpaperPicker *p, QWidget *parent)
    : QWidget(parent), m_p(p)
{
    setMouseTracking(true);
    connect(p, &WallpaperPicker::changed, this, [this]() { update(); });
    connect(p, &WallpaperPicker::selectionChanged, this, [this]() {
        ensureVisible();
        update();
    });
}

int WallpaperGrid::contentHeight() const
{
    const int n = m_p->entries.size();
    if (n == 0)
        return 0;
    const int rows = (n + WallpaperPicker::columns - 1) / WallpaperPicker::columns;
    return rows * WallpaperPicker::cellH();
}

void WallpaperGrid::clampScroll()
{
    m_scroll = qBound(0, m_scroll, qMax(0, contentHeight() - height()));
}

void WallpaperGrid::ensureVisible()
{
    const int row = m_p->selected / WallpaperPicker::columns;
    const int top = row * WallpaperPicker::cellH();
    const int bottom = top + WallpaperPicker::cellH();
    if (top < m_scroll)
        m_scroll = top;
    else if (bottom > m_scroll + height())
        m_scroll = bottom - height();
    clampScroll();
}

int WallpaperGrid::indexAt(const QPoint &pt) const
{
    if (pt.x() < 0 || pt.x() >= WallpaperPicker::columns * WallpaperPicker::cellW())
        return -1;
    const int col = pt.x() / WallpaperPicker::cellW();
    const int row = (pt.y() + m_scroll) / WallpaperPicker::cellH();
    const int i = row * WallpaperPicker::columns + col;
    return (i >= 0 && i < m_p->entries.size()) ? i : -1;
}

void WallpaperGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int cw = WallpaperPicker::cellW();
    const int ch = WallpaperPicker::cellH();
    const int pad = WallpaperPicker::cellPad;

    for (int i = 0; i < m_p->entries.size(); i++) {
        const int row = i / WallpaperPicker::columns;
        const int col = i % WallpaperPicker::columns;
        const QRect cell(col * cw, row * ch - m_scroll, cw, ch);
        if (!cell.intersects(rect()))
            continue;

        const Wallpaper &w = m_p->entries[i];
        const bool sel = (i == m_p->selected);
        const bool active = m_p->isCurrent(w.path);

        if (sel)
            Theme::paintRect(p, cell.adjusted(2, 2, -2, -2), Theme::surface,
                             Theme::radius);

        const QRect thumb(cell.x() + pad, cell.y() + pad,
                          WallpaperPicker::thumbW, WallpaperPicker::thumbH);
        if (!w.thumb.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(thumb, Theme::smallRadius, Theme::smallRadius);
            p.save();
            p.setClipPath(clip);
            p.drawPixmap(thumb, w.thumb);
            p.restore();
        } else {
            /* Still decoding, or unreadable. */
            Theme::paintRect(p, thumb, Theme::surfaceHover, Theme::smallRadius);
            p.setPen(Theme::textMuted);
            p.setFont(Theme::font(Theme::smallFontSize));
            p.drawText(thumb, Qt::AlignCenter,
                       w.decoded ? QStringLiteral("unreadable")
                                 : QStringLiteral("…"));
        }

        /* The wallpaper in use gets an accent frame; the keyboard cursor gets
         * the surface fill above. They stack, so you can tell "selected" from
         * "currently applied" at a glance. */
        if (active)
            Theme::paintRect(p, thumb, Theme::transparent, Theme::smallRadius,
                             Theme::accent, 2);

        const QRect label(cell.x() + pad, thumb.bottom() + 1 + pad,
                          WallpaperPicker::thumbW, WallpaperPicker::labelH);
        p.setFont(Theme::font(Theme::smallFontSize, sel || active));
        p.setPen(active ? Theme::accent : (sel ? Theme::text : Theme::textMuted));
        const QFontMetrics fm(p.font());
        p.drawText(label, Qt::AlignHCenter | Qt::AlignVCenter,
                   fm.elidedText(w.name, Qt::ElideMiddle, label.width()));
    }
}

void WallpaperGrid::mouseMoveEvent(QMouseEvent *e)
{
    const int i = indexAt(e->pos());
    if (i >= 0 && i != m_p->selected) {
        m_p->selected = i;
        update();
    }
}

void WallpaperGrid::mousePressEvent(QMouseEvent *e)
{
    const int i = indexAt(e->pos());
    if (i < 0)
        return;
    m_p->selected = i;
    m_p->apply(i);
    m_p->hidePicker();
}

void WallpaperGrid::wheelEvent(QWheelEvent *e)
{
    m_scroll -= e->angleDelta().y();
    clampScroll();
    update();
}

void WallpaperGrid::resizeEvent(QResizeEvent *)
{
    clampScroll();
}

/* ---------------------------------------------------------- WallpaperWindow */

namespace {

/* The "Apply to" row above the grid. */
constexpr int headerH = Theme::buttonHeight + Theme::popupSpacing;
constexpr int targetW = 240;

QRect boxRectFor(const QSize &winSize, int gridContentH)
{
    const int w = WallpaperPicker::columns * WallpaperPicker::cellW()
                  + 2 * Theme::popupMargin;
    const int maxH = qRound(winSize.height() * 0.72);
    const int h = qMin(maxH, 2 * Theme::popupMargin + headerH + gridContentH);
    const int x = (winSize.width() - w) / 2;
    const int y = (winSize.height() - h) / 2;
    return QRect(x, y, w, h);
}

} /* namespace */

WallpaperWindow::WallpaperWindow(WallpaperPicker *p)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), m_p(p)
{
    setWindowTitle(QStringLiteral("fanshell-wallpaper"));
    setAttribute(Qt::WA_TranslucentBackground);
    m_grid = new WallpaperGrid(p, this);
    m_targetLabel = new TextItem(this);
    m_targetLabel->setPixelSize(Theme::smallFontSize);
    m_targetLabel->setColor(Theme::textMuted);
    m_targetLabel->setText(QStringLiteral("Apply to"));
    m_list = new ChoiceList(this);
    m_target = new Selector(m_list, this);
    m_target->chosen = [this](int i) {
        const QStringList names = WallpaperPicker::outputNames();
        m_p->target = i > 0 ? names.value(i - 1) : QString();
        m_p->syncSelectionToCurrent();
        syncTarget();
        m_grid->ensureVisible();
        m_grid->update();
    };
    connect(p, &WallpaperPicker::changed, this, [this]() {
        if (isVisible())
            layoutBox();
    });
}

void WallpaperWindow::syncTarget()
{
    const QStringList names = WallpaperPicker::outputNames();
    m_target->setChoices(QStringList{QStringLiteral("All displays")} + names,
                         m_p->target.isEmpty() ? 0 : int(names.indexOf(m_p->target)) + 1);
}

void WallpaperWindow::openOn(QScreen *screen)
{
    /* An open popup holds keyboard focus; drop it before we take over. */
    PopupManager::closeCurrent();

    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QString focused = WmState::instance()->focusedMonitor();
    if (!focused.isEmpty())
        for (QScreen *s : QGuiApplication::screens())
            if (s->name() == focused)
                screen = s;
    if (!screen)
        return;

    if (m_configured && m_screen != screen) {
        /* Following the focused monitor: re-place on the new one. */
        hide();
        m_configured = false;
    }
    resize(screen->geometry().size());
    if (!m_configured) {
        Surface::SurfaceSpec spec;
        spec.layer = Surface::Layer::Overlay;
        spec.anchors = Surface::AnchorTop | Surface::AnchorBottom
                       | Surface::AnchorLeft | Surface::AnchorRight;
        spec.desiredSize = QSize(0, 0); /* fill the output */
        spec.exclusiveZone = -1;
        spec.keyboard = Surface::Keyboard::Exclusive;
        spec.type = Surface::WindowType::Dialog;
        spec.scope = QStringLiteral("fanshell-qs-wallpaper");
        Surface::configure(this, screen, spec);
        m_screen = screen;
        m_configured = true;
    }
    m_list->hide();
    syncTarget();
    show();
    layoutBox();
    m_grid->ensureVisible();
    setFocus(Qt::OtherFocusReason);
}

void WallpaperWindow::layoutBox()
{
    const QRect box = boxRectFor(size(), m_grid->contentHeight());
    const QRect inner = box.adjusted(Theme::popupMargin, Theme::popupMargin,
                                     -Theme::popupMargin, -Theme::popupMargin);
    const int labelW = m_targetLabel->sizeHint().width();
    m_targetLabel->setGeometry(inner.x(), inner.y(), labelW, Theme::buttonHeight);
    m_target->setGeometry(inner.x() + labelW + Theme::sectionSpacing, inner.y(),
                          targetW, Theme::buttonHeight);
    m_grid->setGeometry(inner.adjusted(0, headerH, 0, 0));
    /* The open list draws over the whole window, grid included. */
    m_list->raise();
    update();
}

void WallpaperWindow::resizeEvent(QResizeEvent *)
{
    layoutBox();
}

void WallpaperWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect box = boxRectFor(size(), m_grid->contentHeight());
    Theme::paintRect(p, box, Theme::bg, Theme::radius, Theme::border, 1);
    if (m_p->entries.isEmpty()) {
        p.setPen(Theme::textMuted);
        p.setFont(Theme::font(Theme::bodyFontSize));
        p.drawText(box, Qt::AlignCenter,
                   QStringLiteral("No images in %1")
                       .arg(WallpaperPicker::wallpaperDir()));
    }
}

void WallpaperWindow::mousePressEvent(QMouseEvent *e)
{
    /* Click on the transparent area outside the box dismisses; clicks inside
     * are the grid's business. */
    if (!boxRectFor(size(), m_grid->contentHeight()).contains(e->pos()))
        m_p->hidePicker();
}

void WallpaperWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Escape:
        if (m_list->isVisible())
            m_list->hide();
        else
            m_p->hidePicker();
        return;
    case Qt::Key_Left:
        m_p->move(-1);
        return;
    case Qt::Key_Right:
        m_p->move(1);
        return;
    case Qt::Key_Up:
        m_p->move(-WallpaperPicker::columns);
        return;
    case Qt::Key_Down:
        m_p->move(WallpaperPicker::columns);
        return;
    case Qt::Key_Home:
        m_p->move(-m_p->entries.size());
        return;
    case Qt::Key_End:
        m_p->move(m_p->entries.size());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        m_p->apply(m_p->selected);
        m_p->hidePicker();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}
