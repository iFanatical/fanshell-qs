#include "notification.h"

#include "osd.h"

#include <QApplication>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QImage>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

#include "wmstate.h"
#include "popup.h"
#include "theme.h"
#include "surface.h"

namespace {

constexpr int historyLimit = 20;
constexpr int toastWidth = 360;
constexpr int toastGap = 6;
constexpr int toastPadding = 12;
constexpr int toastEdgePadding = 12;
constexpr int toastVerticalOffset = 2;
constexpr int toastHorizontalOffset = 4;
/* Enough transparent space above a toast to contain the close button and its
 * matched range-4 shadow without clipping either. */
constexpr int closeOverhang = 13;
constexpr int closeRadius = 9;
constexpr int rightColumnWidth = 68;
constexpr int notificationImageSize = 56;
constexpr int toastShadowRange = 4;
/* Largest notification image (image-data hint) we will decode. */
constexpr int maxImageSide = 4096;
/* Non-default actions become a row of buttons along the bottom of a toast. */
constexpr int actionHeight = 24;
constexpr int actionGap = 6;
constexpr int maxActionButtons = 3;

/* Bodies may carry the spec's small markup subset (we advertise body-markup),
 * but plenty of senders ignore capabilities and send plain text. Only run the
 * HTML parser when the body actually looks like markup -- otherwise it would
 * eat "a < b" and collapse whitespace -- and keep newlines as line breaks
 * either way, as other notification daemons do. */
QString bodyText(const QString &body)
{
    static const QRegularExpression markup(
        QStringLiteral("<[A-Za-z/!]|&(#[0-9]+|#x[0-9A-Fa-f]+|[A-Za-z]+);"));
    if (!body.contains(markup))
        return body;
    QString html = body;
    html.replace(QLatin1Char('\n'), QLatin1String("<br>"));
    return QTextDocumentFragment::fromHtml(html).toPlainText();
}

QVariant unbox(QVariant v)
{
    if (v.canConvert<QDBusVariant>())
        return v.value<QDBusVariant>().variant();
    return v;
}

QIcon resolveIcon(const QString &name)
{
    if (name.isEmpty())
        return QIcon();
    QString path = name;
    if (path.startsWith(QLatin1String("file://")))
        path = path.mid(7);
    if (QFileInfo(path).isAbsolute() && QFileInfo::exists(path))
        return QIcon(path);
    return QIcon::fromTheme(name);
}

QImage imageFromHints(const QVariantMap &hints)
{
    QVariant path = unbox(hints.value(QStringLiteral("image-path")));
    if (!path.isValid())
        path = unbox(hints.value(QStringLiteral("image_path")));
    if (path.isValid()) {
        QString imagePath = path.toString();
        if (imagePath.startsWith(QLatin1String("file://")))
            imagePath = imagePath.mid(7);
        const QImage image(imagePath);
        if (!image.isNull())
            return image;
    }

    QVariant value = hints.value(QStringLiteral("image-data"));
    if (!value.isValid())
        value = hints.value(QStringLiteral("image_data"));
    if (!value.isValid())
        value = hints.value(QStringLiteral("icon_data"));
    value = unbox(value);
    if (!value.canConvert<QDBusArgument>())
        return QImage();

    int width = 0, height = 0, rowstride = 0;
    bool alpha = false;
    int bits = 0, channels = 0;
    QByteArray bytes;
    const QDBusArgument arg = value.value<QDBusArgument>();
    arg.beginStructure();
    arg >> width >> height >> rowstride >> alpha >> bits >> channels >> bytes;
    arg.endStructure();
    /* Every field is sender-controlled. Bound the dimensions, make each row
     * hold a full line of pixels, and check the buffer in 64-bit arithmetic
     * so an oversized header cannot overflow its way past the size test. */
    if (width <= 0 || height <= 0 || width > maxImageSide
            || height > maxImageSide || bits != 8
            || (channels != 3 && channels != 4)
            || qint64(rowstride) < qint64(width) * channels
            || qint64(bytes.size()) < qint64(rowstride) * height)
        return QImage();
    /* The pixel layout follows channels; the alpha flag only says whether
     * the fourth byte means anything. */
    const QImage::Format format =
        channels == 3 ? QImage::Format_RGB888
                      : (alpha ? QImage::Format_RGBA8888
                               : QImage::Format_RGBX8888);
    return QImage(reinterpret_cast<const uchar *>(bytes.constData()), width,
                  height, rowstride, format).copy();
}

QString timestampText(const Notification &n)
{
    return QLocale().toString(n.receivedAt.time(), QLocale::ShortFormat);
}

struct ActionButton {
    QString key;
    QString label;
    QRect rect;
};

/* The sender's non-default actions, laid out across the bottom of a toast
 * card. History rows get none: once a toast has closed its actions are no
 * longer valid to invoke. */
QVector<ActionButton> actionButtons(const Notification &n, const QRect &card)
{
    QVector<ActionButton> out;
    for (int i = 0; i + 1 < n.actions.size()
                    && out.size() < maxActionButtons; i += 2) {
        if (n.actions[i] == QLatin1String("default"))
            continue;
        const QString label = n.actions[i + 1].trimmed();
        out.push_back({n.actions[i],
                       label.isEmpty() ? n.actions[i] : label, QRect()});
    }
    if (out.isEmpty())
        return out;
    const int left = card.left() + toastPadding;
    const int width = card.width() - toastPadding * 2;
    const int each = (width - actionGap * (out.size() - 1)) / out.size();
    const int top = card.bottom() + 1 - toastPadding - actionHeight;
    for (int i = 0; i < out.size(); ++i)
        out[i].rect = QRect(left + i * (each + actionGap), top, each,
                            actionHeight);
    return out;
}

bool hasActionButtons(const Notification &n)
{
    for (int i = 0; i + 1 < n.actions.size(); i += 2)
        if (n.actions[i] != QLatin1String("default"))
            return true;
    return false;
}

/* Shared row geometry for the toast stack and history view. */
int rowHeight(const Notification &n, int width, bool history = false)
{
    const int shadowWidth = history ? 0 : toastShadowRange * 2;
    const int textWidth = width - shadowWidth - toastPadding * 2
                          - rightColumnWidth - 8
                          - (n.icon.isNull() ? 0 : 42);
    QFontMetrics titleFm(Theme::font(Theme::bodyFontSize, true));
    QFontMetrics bodyFm(Theme::font(Theme::smallFontSize));
    const QRect title = titleFm.boundingRect(QRect(0, 0, textWidth, 1000),
                                             Qt::TextWordWrap, n.summary);
    const QRect body = bodyFm.boundingRect(QRect(0, 0, textWidth, 1000),
                                           Qt::TextWordWrap, n.body);
    const int textHeight = toastPadding * 2 + title.height()
                           + (n.body.isEmpty() ? 0 : 4 + body.height());
    const int sideHeight = toastPadding * 2 + Theme::tinyFontSize + 4
                           + (n.image.isNull() ? 0 : notificationImageSize);
    const int actions =
        (!history && hasActionButtons(n)) ? actionHeight + actionGap : 0;
    const int content = qMax(64, qMax(textHeight, sideHeight)) + actions;
    return content + (history ? 0 : closeOverhang + toastShadowRange);
}

void paintNotification(QPainter &p, const Notification &n, const QRect &outer,
                       bool hovered, bool history, bool closeHovered = false,
                       const QString &hoveredAction = QString())
{
    /* Toasts reserve space above the card for a macOS-style close circle
     * straddling its top edge. History rows stay inside their popup and do
     * not expose that dismiss affordance. */
    const QRect card = history
                           ? outer
                           : outer.adjusted(toastShadowRange, closeOverhang,
                                            -toastShadowRange,
                                            -toastShadowRange);
    const QColor border = n.color.isValid() ? n.color : Theme::accent;
    QColor fill = Theme::bg;
    if (!history) {
        /* Hyprland's range=4 / render_power=3 shadow shape, but with a much
         * softer maximum alpha than the user's 0xee window shadow. Draw from
         * outside inward; the opaque card covers the middle, leaving only a
         * four-pixel cubic falloff around the box. */
        p.setPen(Qt::NoPen);
        for (int spread = toastShadowRange; spread >= 1; --spread) {
            const qreal strength =
                qreal(toastShadowRange - spread + 1) / toastShadowRange;
            QColor shadow = Theme::bg;
            shadow.setAlpha(qRound(Theme::notificationShadowAlpha
                                   * strength * strength * strength));
            p.setBrush(shadow);
            p.drawRoundedRect(card.adjusted(-spread, -spread, spread, spread),
                              Theme::radius + spread,
                              Theme::radius + spread);
        }
        fill.setAlpha(Theme::notificationOpacity);
    }
    Theme::paintRect(p, card, fill, Theme::radius, border, 2);

    int x = card.left() + toastPadding;
    if (!n.icon.isNull()) {
        n.icon.paint(&p, QRect(x, card.top() + toastPadding, 32, 32));
        x += 42;
    }
    const int columnRight = card.right() - toastPadding;
    const int columnLeft = columnRight - rightColumnWidth;
    const int textRight = columnLeft - 8;
    const int textWidth = qMax(1, textRight - x);
    int y = card.top() + toastPadding;
    p.setFont(Theme::font(Theme::bodyFontSize, true));
    p.setPen(n.foreground);
    QRect titleRect = p.fontMetrics().boundingRect(
        QRect(x, y, textWidth, 1000), Qt::TextWordWrap, n.summary);
    p.drawText(titleRect, Qt::TextWordWrap, n.summary);
    y = titleRect.bottom() + 5;
    const QVector<ActionButton> buttons =
        history ? QVector<ActionButton>() : actionButtons(n, card);
    const int textBottom = buttons.isEmpty()
                               ? card.bottom() + 1 - toastPadding
                               : buttons.first().rect.top() - actionGap;
    if (!n.body.isEmpty()) {
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(n.foreground);
        p.drawText(QRect(x, y, textWidth, textBottom - y),
                   Qt::TextWordWrap, n.body);
    }
    if (!buttons.isEmpty()) {
        p.setFont(Theme::font(Theme::smallFontSize, true));
        const QFontMetrics fm(p.font());
        for (const ActionButton &b : buttons) {
            const bool hot = b.key == hoveredAction;
            Theme::paintRect(p, b.rect,
                             hot ? Theme::surfaceHover : Theme::surface,
                             Theme::smallRadius, border, 1);
            p.setPen(hot ? Theme::textStrong : Theme::text);
            p.drawText(b.rect, Qt::AlignCenter,
                       fm.elidedText(b.label, Qt::ElideRight,
                                     b.rect.width() - 8));
        }
    }

    p.setFont(Theme::font(Theme::tinyFontSize));
    p.setPen(Theme::textMuted);
    const int timeHeight = p.fontMetrics().height();
    p.drawText(QRect(columnLeft, card.top() + toastPadding,
                     rightColumnWidth, timeHeight),
               Qt::AlignRight | Qt::AlignVCenter, timestampText(n));
    if (!n.image.isNull()) {
        const QImage scaled = n.image.scaled(
            QSize(notificationImageSize, notificationImageSize),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QRect imageRect(QPoint(), scaled.size());
        imageRect.moveTopRight(QPoint(columnRight,
                                      card.top() + toastPadding
                                          + timeHeight + 5));
        p.drawImage(imageRect, scaled);
    }

    if (hovered && !history) {
        const QPoint c(card.left() + closeRadius + 7, card.top());
        p.setPen(Qt::NoPen);
        /* The button exists only while the toast is hovered, so its matching
         * shadow is painted in the same branch and disappears with it. */
        for (int spread = toastShadowRange; spread >= 1; --spread) {
            const qreal strength =
                qreal(toastShadowRange - spread + 1) / toastShadowRange;
            QColor shadow = Theme::bg;
            shadow.setAlpha(qRound(Theme::notificationShadowAlpha
                                   * strength * strength * strength));
            p.setBrush(shadow);
            p.drawEllipse(c, closeRadius + spread, closeRadius + spread);
        }
        /* A centred internal gradient gives the disc subtle convex weight
         * without directional shading or anything painted outside it. */
        const QPointF center(c);
        QRadialGradient button(center, closeRadius, center);
        if (closeHovered) {
            button.setColorAt(0.0, Theme::textMuted);
            button.setColorAt(0.62, Theme::tagEmpty);
            button.setColorAt(1.0, Theme::brightblack);
        } else {
            button.setColorAt(0.0, Theme::tagEmpty);
            button.setColorAt(0.62, Theme::brightblack);
            button.setColorAt(1.0, Theme::surface);
        }
        p.setBrush(button);
        p.drawEllipse(c, closeRadius, closeRadius);

        /* Draw rather than typeset the X: round-capped antialiased strokes
         * stay crisp and centered regardless of font glyph metrics. */
        QPen mark(closeHovered ? QColor("#ececec") : Theme::bg, 2.0,
                  Qt::SolidLine, Qt::RoundCap);
        p.setPen(mark);
        const int arm = 3;
        p.drawLine(c + QPoint(-arm, -arm), c + QPoint(arm, arm));
        p.drawLine(c + QPoint(arm, -arm), c + QPoint(-arm, arm));
    }
}

class NotificationToastWindow : public QWidget {
public:
    NotificationToastWindow()
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint),
          m_service(NotificationService::instance())
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);
        connect(m_service, &NotificationService::changed, this,
                [this]() { sync(); });
        connect(WmState::instance(), &WmState::changed, this,
                [this]() { sync(); });
        connect(qApp, &QGuiApplication::screenAdded, this,
                [this]() { sync(); });
        connect(qApp, &QGuiApplication::screenRemoved, this,
                [this]() { sync(); });
    }

    void sync()
    {
        m_ids = m_service->visibleIds();
        QScreen *target = nullptr;
        const QString host = WmState::instance()->primaryMonitor();
        for (QScreen *s : QGuiApplication::screens())
            if (s->name() == host)
                target = s;
        if (!target && !QGuiApplication::screens().isEmpty())
            target = QGuiApplication::screens().first();

        if (m_configured && target != m_screen) {
            hide();
            destroy();
            m_configured = false;
        }
        m_screen = target;
        if (m_ids.isEmpty() || !m_screen) {
            hide();
            return;
        }

        m_totalHeight = 0;
        for (uint id : m_ids)
            if (const Notification *n = m_service->find(id))
                m_totalHeight += rowHeight(*n, toastWidth)
                                 + (m_totalHeight ? toastGap : 0);
        const int maxHeight = qMax(80, m_screen->geometry().height()
                                      - Theme::panelHeight
                                      - toastEdgePadding * 2);
        const int h = qMin(m_totalHeight, maxHeight);
        m_scroll = qBound(0, m_scroll, qMax(0, m_totalHeight - h));
        setFixedSize(toastWidth, h);
        if (!m_configured) {
            Surface::SurfaceSpec spec;
            spec.layer = Surface::Layer::Overlay;
            spec.anchors = Surface::AnchorTop | Surface::AnchorRight;
            /* The surface begins at the panel edge; its transparent top
             * overhang leaves room for the close circle. */
            spec.margins = QMargins(0,
                                    Theme::panelHeight - toastVerticalOffset,
                                    toastEdgePadding - toastHorizontalOffset, 0);
            spec.desiredSize = QSize(toastWidth, h);
            spec.exclusiveZone = -1;
            spec.keyboard = Surface::Keyboard::None;
            spec.activateOnShow = false;
            spec.type = Surface::WindowType::Notification;
            spec.scope = QStringLiteral("fanshell-qs-notifications");
            Surface::configure(this, m_screen, spec);
            m_configured = true;
        } else {
            Surface::setDesiredSize(this, QSize(toastWidth, h));
        }
        updateShape();
        show();
        update();
    }

    /* Shape the window to the cards themselves (plus the hovered toast's
     * close circle). The compositor blurs within a window's X shape -- picom
     * cannot mask blur by alpha, which is what Hyprland's ignore_alpha did --
     * so without this the shadow margins and the gaps between stacked toasts
     * would blur as a frosted rectangle. It also makes those gaps
     * click-through. The soft painted shadow falls outside the shape. */
    void updateShape()
    {
        QRegion shape;
        int y = -m_scroll;
        for (uint id : m_ids) {
            const Notification *n = m_service->find(id);
            if (!n)
                continue;
            const int h = rowHeight(*n, width());
            const QRect card = QRect(0, y, width(), h).adjusted(
                toastShadowRange, closeOverhang, -toastShadowRange,
                -toastShadowRange);
            QPainterPath path;
            path.addRoundedRect(QRectF(card), Theme::radius, Theme::radius);
            shape += QRegion(path.toFillPolygon().toPolygon());
            if (id == m_hovered) {
                const QPoint c(card.left() + closeRadius + 7, card.top());
                shape += QRegion(QRect(c.x() - closeRadius - 1,
                                       c.y() - closeRadius - 1,
                                       2 * closeRadius + 3, 2 * closeRadius + 3),
                                 QRegion::Ellipse);
            }
            y += h + toastGap;
        }
        shape &= QRegion(rect());
        if (shape != m_shape) {
            m_shape = shape;
            setMask(shape.isEmpty() ? QRegion(0, 0, 1, 1) : shape);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setClipRect(rect());
        int y = -m_scroll;
        for (uint id : m_ids) {
            const Notification *n = m_service->find(id);
            if (!n)
                continue;
            const int h = rowHeight(*n, width());
            paintNotification(p, *n, QRect(0, y, width(), h),
                              id == m_hovered, false,
                              id == m_hovered && m_closeHovered,
                              id == m_hovered ? m_hoveredAction : QString());
            y += h + toastGap;
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const uint next = idAt(e->pos());
        bool close = false;
        QString action;
        if (next) {
            const QRect card = rectFor(next).adjusted(
                toastShadowRange, closeOverhang, -toastShadowRange,
                -toastShadowRange);
            const QPoint center(card.left() + closeRadius + 7, card.top());
            const QPoint delta = e->pos() - center;
            close = delta.x() * delta.x() + delta.y() * delta.y()
                    <= closeRadius * closeRadius;
            if (!close)
                action = actionAt(next, card, e->pos());
        }
        if (m_hovered != next || m_closeHovered != close
                || m_hoveredAction != action) {
            if (m_hovered != next)
                m_service->setHovered(next);
            m_hovered = next;
            m_closeHovered = close;
            m_hoveredAction = action;
            setCursor((close || !action.isEmpty()) ? Qt::PointingHandCursor
                                                   : Qt::ArrowCursor);
            updateShape();
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = 0;
        m_closeHovered = false;
        m_hoveredAction.clear();
        m_service->setHovered(0);
        setCursor(Qt::ArrowCursor);
        updateShape();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        const uint id = idAt(e->pos());
        const Notification *n = m_service->find(id);
        if (!n)
            return;
        const QRect r = rectFor(id);
        const QRect card = r.adjusted(toastShadowRange, closeOverhang,
                                      -toastShadowRange, -toastShadowRange);
        const QPoint closeCenter(card.left() + closeRadius + 7, card.top());
        const QPoint delta = e->pos() - closeCenter;
        if (delta.x() * delta.x() + delta.y() * delta.y()
                <= closeRadius * closeRadius) {
            m_service->dismiss(id);
            return;
        }
        const QString action = actionAt(id, card, e->pos());
        if (!action.isEmpty())
            m_service->invokeAction(id, action);
        else
            m_service->activate(id);
    }

    void wheelEvent(QWheelEvent *e) override
    {
        const int maxScroll = qMax(0, m_totalHeight - height());
        m_scroll = qBound(0, m_scroll - e->angleDelta().y() / 2, maxScroll);
        updateShape();
        update();
    }

private:
    QString actionAt(uint id, const QRect &card, const QPoint &pos) const
    {
        const Notification *n = m_service->find(id);
        if (!n)
            return QString();
        for (const ActionButton &b : actionButtons(*n, card))
            if (b.rect.contains(pos))
                return b.key;
        return QString();
    }

    QRect rectFor(uint wanted) const
    {
        int y = -m_scroll;
        for (uint id : m_ids) {
            const Notification *n = m_service->find(id);
            if (!n)
                continue;
            const int h = rowHeight(*n, width());
            if (id == wanted)
                return QRect(0, y, width(), h);
            y += h + toastGap;
        }
        return QRect();
    }

    uint idAt(const QPoint &pos) const
    {
        for (uint id : m_ids)
            if (rectFor(id).contains(pos))
                return id;
        return 0;
    }

    NotificationService *m_service;
    QVector<uint> m_ids;
    QScreen *m_screen = nullptr;
    uint m_hovered = 0;
    QRegion m_shape;
    bool m_closeHovered = false;
    QString m_hoveredAction;
    int m_totalHeight = 0;
    int m_scroll = 0;
    bool m_configured = false;
};

NotificationToastWindow *toastWindow()
{
    static NotificationToastWindow *w = new NotificationToastWindow();
    return w;
}

class NotificationHistoryView : public QWidget {
public:
    explicit NotificationHistoryView(QWidget *parent = nullptr)
        : QWidget(parent), m_service(NotificationService::instance())
    {
        setMouseTracking(true);
        connect(m_service, &NotificationService::changed, this,
                [this]() { update(); });
        setMinimumHeight(80);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setClipRect(rect());
        const auto &items = m_service->history();
        if (items.isEmpty()) {
            p.setFont(Theme::font(Theme::bodyFontSize));
            p.setPen(Theme::textMuted);
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("No notifications yet."));
            return;
        }
        int y = -m_scroll;
        for (const Notification &n : items) {
            const int h = rowHeight(n, width(), true);
            paintNotification(p, n, QRect(0, y, width(), h),
                              n.id == m_hovered, true);
            y += h + toastGap;
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        m_hovered = idAt(e->pos());
        update();
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = 0;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            const uint id = idAt(e->pos());
            if (id)
                m_service->activate(id);
        }
    }

    void wheelEvent(QWheelEvent *e) override
    {
        int total = 0;
        for (const Notification &n : m_service->history())
            total += rowHeight(n, width(), true) + toastGap;
        m_scroll = qBound(0, m_scroll - e->angleDelta().y() / 2,
                          qMax(0, total - height()));
        update();
    }

private:
    uint idAt(const QPoint &pos) const
    {
        int y = -m_scroll;
        for (const Notification &n : m_service->history()) {
            const int h = rowHeight(n, width(), true);
            if (QRect(0, y, width(), h).contains(pos))
                return n.id;
            y += h + toastGap;
        }
        return 0;
    }

    NotificationService *m_service;
    uint m_hovered = 0;
    int m_scroll = 0;
};

} /* namespace */

class NotificationHistoryPopup : public ShellPopup {
public:
    explicit NotificationHistoryPopup(QWidget *anchor)
        : ShellPopup(anchor, 380)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                   Theme::popupMargin, Theme::popupMargin);
        layout->setSpacing(Theme::popupSpacing);
        auto *header = new QWidget(this);
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(Theme::rowSpacing);
        auto *title = new TextItem(header);
        title->setText(QStringLiteral("Notifications"));
        title->setBold(true);
        title->setPixelSize(Theme::titleFontSize);
        headerLayout->addWidget(title);
        headerLayout->addStretch(1);
        auto *clear = new ShellButton(QStringLiteral("Clear all"), header);
        headerLayout->addWidget(clear);
        layout->addWidget(header);
        layout->addWidget(new NotificationHistoryView(this), 1);
        NotificationService *service = NotificationService::instance();
        connect(clear, &ShellButton::activated, service,
                &NotificationService::clearAll);
        auto syncClear = [service, clear]() {
            clear->setEnabled(!service->history().isEmpty());
        };
        connect(service, &NotificationService::changed, clear, syncClear);
        syncClear();
        setFixedHeight(500);
    }

protected:
    int popupHeight() const override { return 500; }
};

NotificationService *NotificationService::instance()
{
    static NotificationService *service = new NotificationService();
    return service;
}

NotificationService::NotificationService(QObject *parent) : QObject(parent)
{
    connect(HardwareOsd::instance(), &HardwareOsd::closed, this,
            &NotificationService::NotificationClosed);
    m_expiryTimer = new QTimer(this);
    m_expiryTimer->setSingleShot(true);
    m_expiryTimer->setTimerType(Qt::PreciseTimer);
    connect(m_expiryTimer, &QTimer::timeout, this,
            &NotificationService::tick);
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
}

bool NotificationService::start()
{
    if (m_started)
        return true;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")))
        return false;
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"),
                            this, QDBusConnection::ExportScriptableSlots
                                      | QDBusConnection::ExportScriptableSignals)) {
        bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
        return false;
    }
    m_started = true;
    toastWindow()->sync();
    return true;
}

QStringList NotificationService::GetCapabilities()
{
    return {QStringLiteral("actions"), QStringLiteral("body"),
            QStringLiteral("body-markup"),
            QStringLiteral("icon-static"), QStringLiteral("persistence")};
}

void NotificationService::GetServerInformation(QString &name, QString &vendor,
                                                QString &version,
                                                QString &specVersion)
{
    name = QStringLiteral("fanshell-qs-shell");
    vendor = QStringLiteral("fanshell-qs");
    version = QStringLiteral("1.0");
    specVersion = QStringLiteral("1.3");
}

uint NotificationService::Notify(const QString &appName, uint replacesId,
                                 const QString &appIcon,
                                 const QString &summary, const QString &body,
                                 const QStringList &actions,
                                 const QVariantMap &hints, int expireTimeout)
{
    tick();
    int replaceAt = -1;
    if (replacesId)
        for (int i = 0; i < m_history.size(); i++)
            if (m_history[i].id == replacesId) {
                replaceAt = i;
                break;
            }

    const QVariant urgencyValue =
        unbox(hints.value(QStringLiteral("urgency")));
    const int urgency = urgencyValue.isValid() ? urgencyValue.toInt() : 1;
    HardwareOsd *osd = HardwareOsd::instance();
    const uint candidateId = replaceAt >= 0 ? replacesId : m_nextId++;
    if (m_nextId == 0)
        m_nextId = 1;
    if (osd->showNotification(
            candidateId, appName, summary, bodyText(body), hints,
            urgency))
        return candidateId;

    Notification n;
    n.id = replaceAt >= 0 ? replacesId : candidateId;
    n.appName = appName;
    n.appIcon = appIcon;
    n.summary = summary; /* plain text by spec */
    n.body = bodyText(body);
    n.actions = actions;
    n.hints = hints;
    n.icon = resolveIcon(appIcon);
    n.image = imageFromHints(hints);
    n.receivedAt = QDateTime::currentDateTime();
    if (calledFromDBus()) {
        QDBusReply<uint> pid = QDBusConnection::sessionBus().interface()->servicePid(
            message().service());
        if (pid.isValid())
            n.senderPid = pid.value();
    }
    applyStyle(n, expireTimeout);
    n.visible = !m_dnd || n.urgency == 2;

    if (replaceAt >= 0)
        m_history.removeAt(replaceAt);
    m_history.prepend(n);
    while (m_history.size() > historyLimit) {
        const Notification old = m_history.takeLast();
        if (old.visible)
            emit NotificationClosed(old.id, 2);
    }
    emit changed();
    scheduleExpiry();
    return n.id;
}

void NotificationService::applyStyle(Notification &n, int requestedTimeout)
{
    const QVariant urgency = unbox(n.hints.value(QStringLiteral("urgency")));
    n.urgency = urgency.isValid() ? urgency.toInt() : 1;
    n.color = n.urgency == 2 ? Theme::danger : Theme::accent;
    n.foreground = n.urgency == 2 ? Theme::danger : Theme::textStrong;
    int fallback = n.urgency == 2 ? 0 : 5000;

    const QString app = n.appName.toLower();
    if (app == QLatin1String("volume")) {
        n.color = Theme::success;
        n.foreground = Theme::success;
        fallback = 2000;
    } else if (app == QLatin1String("network")) {
        n.color = QColor(QStringLiteral("#7dcfff"));
        n.foreground = n.color;
        fallback = 3000;
    } else if (app == QLatin1String("brightness")) {
        n.color = Theme::warning;
        n.foreground = Theme::warning;
        fallback = 2000;
    } else if (app == QLatin1String("screenshot")) {
        n.color = Theme::brightmagenta;
        n.foreground = Theme::brightmagenta;
        fallback = 3000;
    } else if (n.summary.contains(QStringLiteral("warn"),
                                  Qt::CaseInsensitive)) {
        n.color = Theme::warning;
        n.foreground = Theme::warning;
        fallback = 8000;
    }
    /* Critical notifications are deliberately sticky regardless of what the
     * sender requested; they also bypass DND. */
    n.remainingMs = n.urgency == 2 ? 0
                                   : (requestedTimeout < 0 ? fallback
                                                          : requestedTimeout);
}

QVector<uint> NotificationService::visibleIds() const
{
    QVector<uint> ids;
    for (const Notification &n : m_history)
        if (n.visible)
            ids.push_back(n.id);
    return ids;
}

const Notification *NotificationService::find(uint id) const
{
    for (const Notification &n : m_history)
        if (n.id == id)
            return &n;
    return nullptr;
}

void NotificationService::CloseNotification(uint id)
{
    dismiss(id, 3);
}

void NotificationService::dismiss(uint id, uint reason)
{
    tick();
    for (Notification &n : m_history) {
        if (n.id != id || !n.visible)
            continue;
        n.visible = false;
        if (m_hovered == id)
            m_hovered = 0;
        emit NotificationClosed(id, reason);
        emit changed();
        scheduleExpiry();
        return;
    }
}

void NotificationService::clearAll()
{
    if (m_history.isEmpty())
        return;
    const QVector<Notification> old = m_history;
    m_history.clear();
    m_hovered = 0;
    m_expiryTimer->stop();
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
    for (const Notification &n : old)
        if (n.visible)
            emit NotificationClosed(n.id, 2);
    emit changed();
}

void NotificationService::activate(uint id)
{
    const Notification *ptr = find(id);
    if (!ptr)
        return;
    const Notification n = *ptr;
    /* A closed notification's actions are dead: NotificationClosed has
     * already been sent, so the sender may have dropped its handlers. From
     * history, just bring the sender forward. */
    if (!n.visible) {
        focusCertainSender(n);
        return;
    }
    bool invoked = false;
    for (int i = 0; i + 1 < n.actions.size(); i += 2) {
        if (n.actions[i] == QLatin1String("default")) {
            emit ActionInvoked(id, QStringLiteral("default"));
            invoked = true;
            break;
        }
    }
    if (!invoked)
        focusCertainSender(n);
    dismiss(id, 2);
}

void NotificationService::invokeAction(uint id, const QString &key)
{
    const Notification *n = find(id);
    if (!n || !n->visible)
        return;
    bool known = false;
    for (int i = 0; i + 1 < n->actions.size(); i += 2)
        known = known || n->actions[i] == key;
    if (!known)
        return;
    emit ActionInvoked(id, key);
    dismiss(id, 2);
}

bool NotificationService::focusCertainSender(const Notification &n)
{
    if (!n.senderPid)
        return false;
    /* Window managers don't track PIDs, so read _NET_WM_PID off each window
     * it manages. Only an unambiguous match is focused. */
    WmState *wm = WmState::instance();
    quintptr window = 0;
    int matches = 0;
    for (quintptr xid : wm->windows()) {
        if (Surface::windowPid(xid) != n.senderPid)
            continue;
        window = xid;
        matches++;
    }
    if (matches != 1)
        return false;
    return wm->focusWindow(window);
}

void NotificationService::setHovered(uint id)
{
    tick();
    m_hovered = id;
    scheduleExpiry();
}

void NotificationService::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int elapsed = int(qBound<qint64>(
        qint64(0), now - m_lastTick,
        qint64(std::numeric_limits<int>::max())));
    m_lastTick = now;
    QVector<uint> expired;
    for (Notification &n : m_history) {
        if (!n.visible || n.urgency == 2 || n.id == m_hovered
                || n.remainingMs <= 0)
            continue;
        n.remainingMs -= elapsed;
        if (n.remainingMs <= 0)
            expired.push_back(n.id);
    }
    if (!expired.isEmpty()) {
        for (uint id : expired) {
            for (Notification &n : m_history) {
                if (n.id != id || !n.visible)
                    continue;
                n.visible = false;
                emit NotificationClosed(id, 1);
                break;
            }
        }
        emit changed();
    }
    scheduleExpiry();
}

void NotificationService::scheduleExpiry()
{
    int nearest = std::numeric_limits<int>::max();
    for (const Notification &n : m_history)
        if (n.visible && n.urgency != 2 && n.id != m_hovered
                && n.remainingMs > 0)
            nearest = qMin(nearest, n.remainingMs);
    if (nearest == std::numeric_limits<int>::max())
        m_expiryTimer->stop();
    else
        m_expiryTimer->start(qMax(1, nearest));
}

void NotificationService::toggleDnd()
{
    setDnd(!m_dnd);
}

void NotificationService::setDnd(bool enabled)
{
    if (m_dnd == enabled)
        return;
    m_dnd = enabled;
    tick();
    if (m_dnd) {
        for (Notification &n : m_history)
            if (n.visible && n.urgency != 2)
                n.visible = false;
    }
    emit changed();
    scheduleExpiry();
}

NotificationWidget::NotificationWidget(QWidget *parent) : BarPill(parent)
{
    m_popup = new NotificationHistoryPopup(this);
    connect(this, &BarPill::clicked, m_popup,
            &ShellPopup::togglePopup);
    connect(this, &BarPill::rightClicked,
            NotificationService::instance(), &NotificationService::toggleDnd);
    connect(NotificationService::instance(), &NotificationService::changed,
            this, &NotificationWidget::sync);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            [this](bool) { sync(); });
    sync();
}

void NotificationWidget::sync()
{
    NotificationService *s = NotificationService::instance();
    setIcon(s->dnd() ? QStringLiteral("") : QStringLiteral(""));
    setLabel(s->dnd() && !s->history().isEmpty()
                 ? QString::number(s->history().size()) : QString());
    setTint(s->dnd() ? Theme::danger : Theme::text);
    setActive(s->dnd() || m_popup->isVisible());
}
