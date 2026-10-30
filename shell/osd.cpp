#include "osd.h"

#include <QApplication>
#include <QDBusVariant>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <QtMath>

#include "wmstate.h"
#include "theme.h"
#include "volume.h"
#include "surface.h"

namespace {

constexpr int osdWidth = 300;
constexpr int osdHeight = 78;
constexpr int osdShadowRange = 4;
constexpr int osdBottomMargin = 64;
constexpr int osdTimeoutMs = 2000;

QVariant unboxOsdValue(QVariant value)
{
    while (value.canConvert<QDBusVariant>()) {
        const QVariant inner = qvariant_cast<QDBusVariant>(value).variant();
        if (!inner.isValid() || inner == value)
            break;
        value = inner;
    }
    return value;
}

int percentage(const QVariantMap &hints, const QString &body)
{
    const QVariant hinted = unboxOsdValue(hints.value(QStringLiteral("value")));
    if (hinted.isValid()) {
        bool ok = false;
        const int value = hinted.toInt(&ok);
        if (ok)
            return qBound(0, value, 100);
    }
    const QRegularExpressionMatch match =
        QRegularExpression(QStringLiteral("(\\d+)\\s*%")).match(body);
    return match.hasMatch() ? qBound(0, match.captured(1).toInt(), 100) : -1;
}

QScreen *trayScreen()
{
    const QString name = WmState::instance()->primaryMonitor();
    for (QScreen *screen : QGuiApplication::screens())
        if (screen->name() == name)
            return screen;
    return QGuiApplication::primaryScreen();
}

} // namespace

HardwareOsd *HardwareOsd::instance()
{
    static HardwareOsd *osd = new HardwareOsd;
    return osd;
}

HardwareOsd::HardwareOsd(QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint),
      m_timer(new QTimer(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        hide();
        const uint old = m_id;
        m_id = 0;
        if (old)
            emit closed(old, 1);
    });
}

void HardwareOsd::showVolume(int percent, bool muted)
{
    m_kind = Kind::Volume;
    m_label = QStringLiteral("Volume");
    m_value = qBound(0, percent, 100);
    m_muted = muted;
    m_showProgress = true;
    m_accent = muted ? Theme::danger : Theme::success;
    m_state = muted ? QStringLiteral("Muted")
                    : QString::number(m_value) + QLatin1Char('%');
    present();
}

void HardwareOsd::showMicrophone(int percent, bool muted)
{
    m_kind = Kind::Microphone;
    m_label = QStringLiteral("Microphone");
    m_value = qBound(0, percent, 100);
    m_muted = muted;
    m_showProgress = true;
    m_accent = muted ? Theme::danger : Theme::success;
    m_state = muted ? QStringLiteral("Muted")
                    : QString::number(m_value) + QLatin1Char('%');
    present();
}

void HardwareOsd::showBrightness(int percent, bool external)
{
    m_kind = Kind::Brightness;
    m_label = external ? QStringLiteral("Monitor brightness")
                       : QStringLiteral("Brightness");
    m_value = qBound(0, percent, 100);
    m_muted = false;
    m_showProgress = true;
    m_accent = Theme::warning;
    m_state = QString::number(m_value) + QLatin1Char('%');
    present();
}

void HardwareOsd::showKeyboardBacklight(int percent)
{
    m_kind = Kind::Keyboard;
    m_label = QStringLiteral("Keyboard backlight");
    m_value = qBound(0, percent, 100);
    m_muted = false;
    m_showProgress = true;
    m_accent = Theme::brightmagenta;
    m_state = QString::number(m_value) + QLatin1Char('%');
    present();
}

void HardwareOsd::showLock(const QString &name, const QString &glyph, bool on)
{
    m_kind = Kind::Lock;
    m_label = name;
    m_lockGlyph = glyph;
    m_value = on ? 100 : 0;
    m_muted = false;
    m_showProgress = false;
    m_accent = on ? Theme::accent : Theme::textMuted;
    m_state = on ? QStringLiteral("On") : QStringLiteral("Off");
    present();
}

void HardwareOsd::showVpn(bool connected, const QString &interfaceName,
                          const QString &ipAddress)
{
    m_kind = Kind::Vpn;
    m_label = connected ? QStringLiteral("VPN connected")
                        : QStringLiteral("VPN disconnected");
    m_value = connected ? 100 : 0;
    m_muted = !connected;
    m_showProgress = false;
    m_accent = connected ? Theme::accent : Theme::textMuted;
    if (connected && !ipAddress.isEmpty())
        m_state = ipAddress;
    else if (!interfaceName.isEmpty())
        m_state = interfaceName;
    else
        m_state = connected ? QStringLiteral("Protected")
                            : QStringLiteral("Off");
    present();
}

void HardwareOsd::showMedia(const QString &action, const QString &title,
                            const QString &artist)
{
    m_kind = Kind::Media;
    m_mediaAction = action;
    m_label = title.isEmpty() ? QStringLiteral("Unknown track") : title;
    m_state = artist.isEmpty() ? action : artist;
    m_muted = false;
    m_showProgress = false;
    m_accent = Theme::accent;
    present();
}

void HardwareOsd::showMediaUnavailable()
{
    m_kind = Kind::Media;
    m_mediaAction = QStringLiteral("Unavailable");
    m_label = QStringLiteral("No media player");
    m_state = QStringLiteral("Nothing is available over MPRIS");
    m_muted = false;
    m_showProgress = false;
    m_accent = Theme::textMuted;
    present();
}

bool HardwareOsd::s_suppressed = false;

void HardwareOsd::setSuppressed(bool suppressed)
{
    s_suppressed = suppressed;
    if (suppressed)
        instance()->hide();
}

void HardwareOsd::present()
{
    if (s_suppressed)
        return;
    configureFor(trayScreen());
    show();
    raise();
    update();
    m_timer->start(osdTimeoutMs);
}

bool HardwareOsd::showNotification(uint id, const QString &appName,
                                   const QString &summary,
                                   const QString &body,
                                   const QVariantMap &hints, int urgency)
{
    if (urgency == 2)
        return false;

    const QString app = appName.trimmed().toLower();
    const QString title = summary.trimmed();
    const QString titleLower = title.toLower();
    if (app == QLatin1String("volume")) {
        m_kind = titleLower.contains(QLatin1String("microphone"))
                     ? Kind::Microphone : Kind::Volume;
    } else if (app == QLatin1String("brightness")) {
        m_kind = titleLower.contains(QLatin1String("keyboard"))
                     ? Kind::Keyboard : Kind::Brightness;
    } else if (app == QLatin1String("keyboard-brightness")
               || app == QLatin1String("keyboard_backlight")) {
        m_kind = Kind::Keyboard;
    } else {
        return false;
    }

    const QString state = body.trimmed();
    m_muted = state.compare(QLatin1String("muted"),
                            Qt::CaseInsensitive) == 0;
    m_value = percentage(hints, body);
    PulseBackend *pulse = PulseBackend::instance();
    if (m_value < 0 && m_kind == Kind::Volume && pulse->hasSink)
        m_value = qRound(pulse->sinkVolume * 100.0);
    if (m_value < 0 && m_kind == Kind::Microphone && pulse->hasSource)
        m_value = qRound(pulse->sourceVolume * 100.0);
    if (m_value < 0 && !m_muted)
        return false;
    m_value = qBound(0, m_value < 0 ? 0 : m_value, 100);

    switch (m_kind) {
    case Kind::Volume:
        m_label = QStringLiteral("Volume");
        m_accent = m_muted ? Theme::danger : Theme::success;
        break;
    case Kind::Microphone:
        m_label = QStringLiteral("Microphone");
        m_accent = m_muted ? Theme::danger : Theme::success;
        break;
    case Kind::Brightness:
        m_label = titleLower.contains(QLatin1String("monitor"))
                      ? QStringLiteral("Monitor brightness")
                      : QStringLiteral("Brightness");
        m_accent = Theme::warning;
        break;
    case Kind::Keyboard:
        m_label = QStringLiteral("Keyboard backlight");
        m_accent = Theme::brightmagenta;
        break;
    case Kind::Lock:
    case Kind::Vpn:
    case Kind::Media:
        break;
    }
    m_showProgress = true;
    m_state = m_muted ? QStringLiteral("Muted")
                      : QString::number(m_value) + QLatin1Char('%');

    if (m_id && m_id != id)
        emit closed(m_id, 2);
    m_id = id;
    present();
    return true;
}

void HardwareOsd::configureFor(QScreen *screen)
{
    if (!screen)
        return;
    if (m_configured && screen != m_screen) {
        hide();
        destroy();
        m_configured = false;
    }
    m_screen = screen;
    setFixedSize(osdWidth, osdHeight);
    /* Shape the window to the card, as the notification toasts do: picom
     * blurs within the X shape, so without this the shadow margin would blur
     * as a square-cornered frosted box. The painted shadow falls outside. */
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(osdShadowRange, osdShadowRange,
                                                -osdShadowRange, -osdShadowRange),
                        Theme::radius, Theme::radius);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
    if (m_configured)
        return;
    Surface::SurfaceSpec spec;
    spec.layer = Surface::Layer::Overlay;
    spec.anchors = Surface::AnchorBottom;
    spec.margins = QMargins(0, 0, 0, osdBottomMargin);
    spec.desiredSize = QSize(osdWidth, osdHeight);
    spec.exclusiveZone = -1;
    spec.keyboard = Surface::Keyboard::None;
    spec.activateOnShow = false;
    spec.type = Surface::WindowType::Notification;
    spec.scope = QStringLiteral("fanshell-qs-osd");
    Surface::configure(this, screen, spec);
    m_configured = true;
}

void HardwareOsd::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF card = QRectF(rect()).adjusted(osdShadowRange,
                                                osdShadowRange,
                                                -osdShadowRange,
                                                -osdShadowRange);
    p.setPen(Qt::NoPen);
    for (int spread = osdShadowRange; spread >= 1; --spread) {
        const qreal strength =
            qreal(osdShadowRange - spread + 1) / osdShadowRange;
        QColor shadow = Theme::bg;
        shadow.setAlpha(qRound(Theme::notificationShadowAlpha
                               * strength * strength * strength));
        p.setBrush(shadow);
        p.drawRoundedRect(card.adjusted(-spread, -spread, spread, spread),
                          Theme::radius + spread, Theme::radius + spread);
    }
    QColor fill = Theme::bg;
    fill.setAlpha(Theme::notificationOpacity);
    Theme::paintRect(p, card, fill, Theme::radius, m_accent, 2);

    drawIcon(p, QRectF(card.left() + 16, card.top() + 18, 34, 34));
    const qreal left = card.left() + 64;
    const qreal right = card.right() - 16;
    const qreal textY = m_showProgress ? card.top() + 11
                        : m_kind == Kind::Media ? card.top() + 10
                                               : card.top() + (card.height() - 20) / 2;
    p.setFont(Theme::font(Theme::smallFontSize, true));
    p.setPen(Theme::textStrong);
    const QFontMetrics titleMetrics(p.font());
    const QString visibleLabel = titleMetrics.elidedText(
        m_label, Qt::ElideRight, qRound(right - left));
    p.drawText(QRectF(left, textY, right - left, 20),
               Qt::AlignLeft | Qt::AlignVCenter, visibleLabel);
    p.setFont(Theme::font(Theme::smallFontSize));
    p.setPen(m_accent);
    if (m_kind == Kind::Media) {
        const QFontMetrics stateMetrics(p.font());
        const QString visibleState = stateMetrics.elidedText(
            m_state, Qt::ElideRight, qRound(right - left));
        p.drawText(QRectF(left, textY + 24, right - left, 18),
                   Qt::AlignLeft | Qt::AlignVCenter, visibleState);
    } else {
        p.drawText(QRectF(left, textY, right - left, 20),
                   Qt::AlignRight | Qt::AlignVCenter, m_state);
    }

    if (m_showProgress) {
        const QRectF track(left, card.top() + 43, right - left, 8);
        Theme::paintRect(p, track, Theme::surface, 4);
        if (!m_muted && m_value > 0) {
            QRectF level = track;
            level.setWidth(track.width() * m_value / 100.0);
            Theme::paintRect(p, level, m_accent, 4);
        }
    }
}

void HardwareOsd::drawIcon(QPainter &p, const QRectF &r) const
{
    p.save();
    QPen pen(m_accent, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    if (m_kind == Kind::Media) {
        if (m_mediaAction == QLatin1String("Previous")) {
            p.drawLine(QPointF(c.x() - 9, c.y() - 10),
                       QPointF(c.x() - 9, c.y() + 10));
            QPainterPath triangle;
            triangle.moveTo(c.x() + 9, c.y() - 10);
            triangle.lineTo(c.x() - 6, c.y());
            triangle.lineTo(c.x() + 9, c.y() + 10);
            triangle.closeSubpath();
            p.drawPath(triangle);
        } else if (m_mediaAction == QLatin1String("Next")) {
            QPainterPath triangle;
            triangle.moveTo(c.x() - 9, c.y() - 10);
            triangle.lineTo(c.x() + 6, c.y());
            triangle.lineTo(c.x() - 9, c.y() + 10);
            triangle.closeSubpath();
            p.drawPath(triangle);
            p.drawLine(QPointF(c.x() + 9, c.y() - 10),
                       QPointF(c.x() + 9, c.y() + 10));
        } else if (m_mediaAction == QLatin1String("Pause")) {
            p.drawLine(QPointF(c.x() - 6, c.y() - 10),
                       QPointF(c.x() - 6, c.y() + 10));
            p.drawLine(QPointF(c.x() + 6, c.y() - 10),
                       QPointF(c.x() + 6, c.y() + 10));
        } else {
            QPainterPath triangle;
            triangle.moveTo(c.x() - 7, c.y() - 11);
            triangle.lineTo(c.x() + 10, c.y());
            triangle.lineTo(c.x() - 7, c.y() + 11);
            triangle.closeSubpath();
            p.drawPath(triangle);
        }
    } else if (m_kind == Kind::Vpn) {
        QPainterPath shield;
        shield.moveTo(c.x(), r.top() + 2);
        shield.cubicTo(r.right() - 6, r.top() + 6,
                       r.right() - 5, r.top() + 8,
                       r.right() - 6, c.y() + 3);
        shield.cubicTo(r.right() - 8, r.bottom() - 5,
                       c.x(), r.bottom() - 2,
                       c.x(), r.bottom() - 2);
        shield.cubicTo(c.x(), r.bottom() - 2,
                       r.left() + 8, r.bottom() - 5,
                       r.left() + 6, c.y() + 3);
        shield.cubicTo(r.left() + 5, r.top() + 8,
                       r.left() + 6, r.top() + 6,
                       c.x(), r.top() + 2);
        p.drawPath(shield);
        if (!m_muted) {
            p.drawLine(QPointF(c.x() - 7, c.y()),
                       QPointF(c.x() - 2, c.y() + 5));
            p.drawLine(QPointF(c.x() - 2, c.y() + 5),
                       QPointF(c.x() + 8, c.y() - 6));
        }
    } else if (m_kind == Kind::Brightness) {
        p.drawEllipse(c, 6, 6);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            p.drawLine(c + QPointF(qCos(a) * 10, qSin(a) * 10),
                       c + QPointF(qCos(a) * 15, qSin(a) * 15));
        }
    } else if (m_kind == Kind::Keyboard) {
        p.drawRoundedRect(r.adjusted(2, 7, -2, -7), 3, 3);
        p.drawLine(QPointF(r.left() + 8, r.bottom() - 11),
                   QPointF(r.right() - 8, r.bottom() - 11));
        for (int i = 0; i < 3; ++i)
            p.drawLine(QPointF(r.left() + 9 + i * 8, r.top() + 13),
                       QPointF(r.left() + 12 + i * 8, r.top() + 13));
    } else if (m_kind == Kind::Lock) {
        /* A key cap with its letter. */
        p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 6, 6);
        p.setFont(Theme::font(17, true));
        p.drawText(r.adjusted(3, 2, -3, -3), Qt::AlignCenter, m_lockGlyph);
    } else if (m_kind == Kind::Microphone) {
        p.drawRoundedRect(QRectF(c.x() - 5, r.top() + 3, 10, 18), 5, 5);
        p.drawArc(QRectF(c.x() - 10, r.top() + 8, 20, 18), 180 * 16,
                  180 * 16);
        p.drawLine(QPointF(c.x(), r.bottom() - 8),
                   QPointF(c.x(), r.bottom() - 3));
        p.drawLine(QPointF(c.x() - 6, r.bottom() - 3),
                   QPointF(c.x() + 6, r.bottom() - 3));
    } else {
        QPainterPath speaker;
        speaker.moveTo(r.left() + 3, c.y() - 5);
        speaker.lineTo(r.left() + 10, c.y() - 5);
        speaker.lineTo(r.left() + 17, c.y() - 12);
        speaker.lineTo(r.left() + 17, c.y() + 12);
        speaker.lineTo(r.left() + 10, c.y() + 5);
        speaker.lineTo(r.left() + 3, c.y() + 5);
        speaker.closeSubpath();
        p.drawPath(speaker);
        if (!m_muted) {
            p.drawArc(QRectF(r.left() + 12, c.y() - 9, 16, 18), -60 * 16,
                      120 * 16);
            if (m_value > 50)
                p.drawArc(QRectF(r.left() + 9, c.y() - 14, 26, 28),
                          -55 * 16, 110 * 16);
        }
    }
    if (m_muted && m_kind != Kind::Vpn) {
        p.setPen(QPen(Theme::danger, 2.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(r.topLeft() + QPointF(4, 4),
                   r.bottomRight() - QPointF(4, 4));
    }
    p.restore();
}
