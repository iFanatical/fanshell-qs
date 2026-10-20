#include "capture.h"

#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

#include "notification.h"
#include "paths.h"
#include "surface.h"
#include "theme.h"

namespace {

/* The loupe: this many pixels each way from the pointer, this big each. */
constexpr int loupeRadius = 7;
constexpr int loupeZoom = 10;

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

/* The whole screen as picom shows it: the root window with its inferiors,
 * which include the compositor's overlay window. Null unless it is the
 * usual 24-bit, 32 bits-per-pixel little-endian kind. */
QImage grabScreen()
{
    xcb_connection_t *c = conn();
    if (!c)
        return QImage();
    const xcb_setup_t *setup = xcb_get_setup(c);
    const xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    if (setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST)
        return QImage();
    QSize size;
    if (xcb_get_geometry_reply_t *g =
            xcb_get_geometry_reply(c, xcb_get_geometry(c, screen->root), nullptr)) {
        size = QSize(g->width, g->height);
        free(g);
    }
    const int w = size.width(), h = size.height();
    if (w <= 0 || h <= 0)
        return QImage();
    xcb_get_image_reply_t *img = xcb_get_image_reply(
        c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, screen->root, 0, 0,
                         uint16_t(w), uint16_t(h), ~0u),
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

QScreen *screenAt(const QPoint &global)
{
    for (QScreen *s : QGuiApplication::screens())
        if (s->geometry().contains(global))
            return s;
    return QGuiApplication::primaryScreen();
}

void notify(const QString &summary, const QString &body, const QString &imagePath)
{
    QVariantMap hints;
    if (!imagePath.isEmpty())
        hints.insert(QStringLiteral("image-path"), imagePath);
    NotificationService::instance()->Notify(QStringLiteral("screenshot"), 0, QString(),
                                            summary, body, {}, hints, -1);
}

QString runtimeFile(const QString &name)
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return dir + QLatin1Char('/') + name;
}

} /* namespace */

/* One output's part of the overlay: the frozen screen, with the loupe or
 * the selection drawn over it. */
class CaptureWindow : public QWidget {
public:
    CaptureWindow(Capture *capture, QScreen *screen, bool keyboard)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), m_capture(capture),
          m_origin(screen->geometry().topLeft())
    {
        setWindowTitle(QStringLiteral("fanshell-qs-capture"));
        /* An alpha channel, so picom does not take it for an opaque
         * fullscreen window and stop compositing (unredir-if-fullscreen). */
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        resize(screen->geometry().size());
        Surface::SurfaceSpec spec;
        spec.layer = Surface::Layer::Overlay;
        spec.anchors = Surface::AnchorTop | Surface::AnchorBottom | Surface::AnchorLeft
                       | Surface::AnchorRight;
        spec.desiredSize = QSize(0, 0);
        spec.exclusiveZone = -1;
        spec.keyboard = keyboard ? Surface::Keyboard::Exclusive : Surface::Keyboard::None;
        spec.type = Surface::WindowType::Dialog;
        spec.scope = QStringLiteral("fanshell-qs-capture");
        Surface::configure(this, screen, spec);
    }

    QPoint origin() const { return m_origin; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect here(m_origin, size());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(0, 0), m_capture->m_frozen, here);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (m_capture->m_kind == Capture::Kind::Pick)
            paintLoupe(p);
        else
            paintSelection(p, here);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::RightButton)
            m_capture->cancel();
        else if (e->button() == Qt::LeftButton)
            m_capture->pressed(e->globalPosition().toPoint());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        m_capture->moved(e->globalPosition().toPoint());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            m_capture->released(e->globalPosition().toPoint());
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            m_capture->cancel();
    }

private:
    void paintSelection(QPainter &p, const QRect &here)
    {
        const QRect sel = m_capture->selection();
        QRegion dim(rect());
        if (sel.isValid())
            dim -= sel.translated(-m_origin);
        p.setClipRegion(dim);
        p.fillRect(rect(), QColor(0, 0, 0, 110));
        p.setClipping(false);
        if (!sel.isValid() || !sel.intersects(here))
            return;
        const QRect local = sel.translated(-m_origin);
        p.setPen(QPen(Theme::accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(local.adjusted(-1, -1, 0, 0));
        const QString label = QStringLiteral("%1 × %2").arg(sel.width()).arg(sel.height());
        p.setFont(Theme::font(Theme::smallFontSize, true));
        const QRect text = p.fontMetrics().boundingRect(label).adjusted(-6, -3, 6, 3);
        QRect box(local.left(), local.top() - text.height() - 4, text.width(), text.height());
        if (box.top() < 0)
            box.moveTop(local.top() + 4);
        Theme::paintRect(p, box, Theme::bg, Theme::smallRadius);
        p.setPen(Theme::textStrong);
        p.drawText(box, Qt::AlignCenter, label);
    }

    void paintLoupe(QPainter &p)
    {
        const QPoint at = m_capture->m_pointer;
        if (!QRect(m_origin, size()).contains(at))
            return;
        const QImage &img = m_capture->m_frozen;
        const int side = (2 * loupeRadius + 1) * loupeZoom;
        const QPoint local = at - m_origin;
        /* Beside the pointer, flipped near the edges. */
        QRect box(local + QPoint(24, 24), QSize(side, side + 28));
        if (box.right() > width())
            box.moveRight(local.x() - 24);
        if (box.bottom() > height())
            box.moveBottom(local.y() - 24);
        const QRect zoom(box.topLeft(), QSize(side, side));
        Theme::paintRect(p, box.adjusted(-4, -4, 4, 4), Theme::bg, Theme::radius,
                         Theme::border, 1);
        p.save();
        p.setClipRect(zoom);
        for (int dy = -loupeRadius; dy <= loupeRadius; dy++)
            for (int dx = -loupeRadius; dx <= loupeRadius; dx++) {
                const QPoint src = at + QPoint(dx, dy);
                const QColor c = img.rect().contains(src) ? img.pixelColor(src) : Qt::black;
                p.fillRect(zoom.x() + (dx + loupeRadius) * loupeZoom,
                           zoom.y() + (dy + loupeRadius) * loupeZoom, loupeZoom, loupeZoom,
                           c);
            }
        p.restore();
        const QRect centre(zoom.x() + loupeRadius * loupeZoom,
                           zoom.y() + loupeRadius * loupeZoom, loupeZoom, loupeZoom);
        p.setPen(QPen(Qt::white, 1));
        p.drawRect(centre.adjusted(0, 0, -1, -1));
        p.setPen(QPen(Qt::black, 1));
        p.drawRect(centre.adjusted(-1, -1, 0, 0));

        const QColor c = img.rect().contains(at) ? img.pixelColor(at) : Qt::black;
        const QRect info(box.x(), zoom.bottom() + 5, side, 22);
        p.fillRect(QRect(info.x(), info.y() + 3, 16, 16), c);
        p.setPen(Theme::textStrong);
        p.setFont(Theme::font(Theme::smallFontSize, true));
        p.drawText(info.adjusted(22, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   c.name().toUpper());
    }

    Capture *m_capture;
    QPoint m_origin;
};

Capture *Capture::instance()
{
    static Capture *s = new Capture();
    return s;
}

Capture::Capture(QObject *parent) : QObject(parent) {}

void Capture::begin(Kind kind)
{
    if (!m_windows.isEmpty())
        return;
    m_frozen = grabScreen();
    if (m_frozen.isNull()) {
        notify(QStringLiteral("Cannot capture the screen"), QString(), QString());
        return;
    }
    m_kind = kind;
    m_pointer = QCursor::pos();
    m_dragging = false;

    if (kind == Kind::Screen) {
        finishShot(m_frozen.rect());
        return;
    }
    if (kind == Kind::Output) {
        finishShot(screenAt(m_pointer)->geometry());
        return;
    }
    /* The keyboard (for Escape) goes to the output under the pointer. */
    QScreen *focus = screenAt(m_pointer);
    for (QScreen *s : QGuiApplication::screens()) {
        auto *w = new CaptureWindow(this, s, s == focus);
        m_windows << w;
        w->show();
    }
}

QRect Capture::selection() const
{
    if (!m_dragging)
        return QRect();
    /* From the pixel pressed to the one released, that one left out. */
    const QPoint a = m_anchor, b = m_pointer;
    return QRect(QPoint(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                 QSize(qAbs(b.x() - a.x()), qAbs(b.y() - a.y())));
}

void Capture::pressed(const QPoint &global)
{
    m_pointer = global;
    if (m_kind == Kind::Pick) {
        finishPick(global);
        return;
    }
    m_anchor = global;
    m_dragging = true;
    for (CaptureWindow *w : std::as_const(m_windows))
        w->update();
}

void Capture::moved(const QPoint &global)
{
    m_pointer = global;
    for (CaptureWindow *w : std::as_const(m_windows))
        w->update();
}

void Capture::released(const QPoint &global)
{
    if (m_kind != Kind::Region || !m_dragging)
        return;
    m_pointer = global;
    const QRect sel = selection();
    /* A click rather than a drag: the whole output. */
    finishShot(sel.width() < 4 || sel.height() < 4 ? screenAt(global)->geometry() : sel);
}

void Capture::cancel()
{
    close();
}

void Capture::close()
{
    for (CaptureWindow *w : std::as_const(m_windows)) {
        w->hide();
        w->deleteLater();
    }
    m_windows.clear();
    m_frozen = QImage();
    m_dragging = false;
}

void Capture::finishPick(const QPoint &global)
{
    const QColor c = m_frozen.rect().contains(global) ? m_frozen.pixelColor(global)
                                                      : QColor(Qt::black);
    close();
    const QString hex = c.name();
    QGuiApplication::clipboard()->setText(hex);
    QImage swatch(64, 64, QImage::Format_RGB32);
    swatch.fill(c);
    const QString path = runtimeFile(QStringLiteral("fanshell-qs-colour.png"));
    swatch.save(path);
    notify(QStringLiteral("Colour %1 copied").arg(hex.toUpper()),
           QStringLiteral("rgb(%1, %2, %3)").arg(c.red()).arg(c.green()).arg(c.blue()),
           path);
}

void Capture::finishShot(const QRect &global)
{
    const QImage shot = m_frozen.copy(global & m_frozen.rect());
    close();
    if (shot.isNull())
        return;
    const QString dir = Paths::setting(
        QStringLiteral("screenshot.dir"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
            + QStringLiteral("/screenshots"));
    QDir().mkpath(dir);
    const QString name = QDateTime::currentDateTime().toString(
                             QStringLiteral("yyyy-MM-dd-HHmmss-zzz"))
                         + QStringLiteral("_screenshot.png");
    const QString path = dir + QLatin1Char('/') + name;
    if (!shot.save(path, "PNG")) {
        notify(QStringLiteral("Cannot save the screenshot"), path, QString());
        return;
    }
    QGuiApplication::clipboard()->setImage(shot);
    notify(QStringLiteral("Screenshot saved"),
           QStringLiteral("%1, copied to the clipboard").arg(name), path);
}
