#include "pills.h"

#include "wmstate.h"
#include "notification.h"
#include "osd.h"
#include "paths.h"

#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

static QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

/* -------------------------------------------------------------- ScriptPill */

ScriptPill::ScriptPill(const QStringList &command, int intervalMs,
                       QWidget *parent)
    : BarPill(parent)
{
    m_proc.setCommand(command);
    connect(&m_proc, &CollectorProcess::finished, this, &ScriptPill::parse);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, &m_proc, [this]() { m_proc.start(); });
    timer->start(intervalMs);
    m_proc.start(); /* triggeredOnStart */

    hide(); /* hidden until the script reports something */
}

void ScriptPill::setFixedTint(const QColor &c)
{
    m_fixedTint = c;
    m_hasFixedTint = true;
    setTint(c);
}

void ScriptPill::parse(const QString &out)
{
    QString ic, tx, cl;
    const QStringList lines = out.trimmed().split('\n');
    for (const QString &line : lines) {
        const int s = line.indexOf('=');
        if (s < 0)
            continue;
        const QString k = line.left(s), v = line.mid(s + 1);
        if (k == QLatin1String("icon"))
            ic = v;
        else if (k == QLatin1String("text"))
            tx = v;
        else if (k == QLatin1String("color"))
            cl = v;
    }
    setIcon(ic);
    setLabel(tx);
    if (!m_hasFixedTint)
        setTint(cl.isEmpty() ? Theme::text : QColor(cl));
    setVisible(!tx.isEmpty() || !ic.isEmpty()); /* hideWhenEmpty */
}

/* ------------------------------------------------------------ LayoutWidget */

LayoutWidget::LayoutWidget(QScreen *screen, QWidget *parent)
    : BarPill(parent), m_screen(screen)
{
    setIcon(QStringLiteral("\U000F056E")); /* mdi view-dashboard */
    setTint(Theme::textMuted);
    connect(this, &BarPill::clicked, this, [this]() {
        WmState *state = WmState::instance();
        if (!m_screen || !state->features().cycleLayout)
            return;
        if (const WmState::Monitor *m = state->monitorByName(m_screen->name()))
            state->cycleLayout(m->id);
    });
    connect(WmState::instance(), &WmState::changed, this,
            &LayoutWidget::sync);
    sync();
}

void LayoutWidget::sync()
{
    const WmState::Monitor *m =
        m_screen ? WmState::instance()->monitorByName(m_screen->name())
                 : nullptr;
    setLabel(m ? m->layout : QString());
    setVisible(m != nullptr && !m->layout.isEmpty());
}

/* ------------------------------------------------------------ SubmapWidget */

SubmapWidget::SubmapWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(QStringLiteral("\U000F030C")); /* mdi keyboard */
    connect(this, &BarPill::clicked, this,
            []() { WmState::instance()->resetSubmap(); });
    connect(WmState::instance(), &WmState::changed, this, &SubmapWidget::sync);
    sync();
}

void SubmapWidget::sync()
{
    WmState *state = WmState::instance();
    const QString name = state->submap;
    const bool active = !name.isEmpty();
    setLabel(active ? name : QStringLiteral("global"));
    /* Accented so a modal keybind mode is hard to miss; muted otherwise. */
    setTint(active ? Theme::accent : Theme::textMuted);
    setActive(active);
    setVisible(state->features().submaps);
}

/* ---------------------------------------------------------------- VpnState */

VpnState *VpnState::instance()
{
    static VpnState *s = new VpnState();
    return s;
}

VpnState::VpnState(QObject *parent) : QObject(parent)
{
    bool countOk = false;
    int count = Paths::setting(QStringLiteral("vpn.tunnels")).toInt(&countOk);
    if (!countOk || count < 1)
        count = 1;
    count = qMin(count, 32);
    for (int i = 1; i <= count; ++i) {
        const QString name = QStringLiteral("tun%1").arg(i);
        m_tunnels.push_back(name);
        m_up.insert(name, false);
    }
    m_statusProc.setCommand({"fanshell-qs-vpn", "status"});
    connect(&m_statusProc, &CollectorProcess::finished, this,
            [this](const QString &text) { parse(text, false); });
    connect(&m_toggleProc, &CollectorProcess::finished, this,
            [this](const QString &text) { parse(text, true); });
    refresh(); /* once at startup; no interval polling */
}

void VpnState::refresh()
{
    m_statusProc.start();
}

void VpnState::toggle()
{
    select(activeTunnel().isEmpty() ? m_tunnels.first()
                                    : activeTunnel());
}

void VpnState::select(const QString &tunnel)
{
    if (!m_tunnels.contains(tunnel))
        return;
    m_toggleProc.start({QStringLiteral("fanshell-qs-vpn"),
                        QStringLiteral("select"), tunnel});
}

bool VpnState::isUp(const QString &tunnel) const
{
    return m_up.value(tunnel, false);
}

QString VpnState::address(const QString &tunnel) const
{
    return m_ips.value(tunnel);
}

QString VpnState::activeTunnel() const
{
    for (const QString &tunnel : m_tunnels)
        if (m_up.value(tunnel))
            return tunnel;
    return {};
}

void VpnState::parse(const QString &text, bool fromToggle)
{
    const bool wasUp = !activeTunnel().isEmpty();
    bool resultKnown = false;
    bool succeeded = false;
    QString interfaceName;
    const QStringList lines = text.trimmed().split('\n');
    for (const QString &l : lines) {
        const int i = l.indexOf('=');
        if (i < 0)
            continue;
        const QString k = l.left(i), v = l.mid(i + 1);
        if (m_up.contains(k))
            m_up[k] = (v == QLatin1String("up"));
        else if (k.endsWith(QLatin1String("_ip"))
                 && m_up.contains(k.chopped(3)))
            m_ips[k.chopped(3)] = v;
        else if (k == QLatin1String("iface"))
            interfaceName = v;
        else if (k == QLatin1String("result")) {
            resultKnown = true;
            succeeded = (v == QLatin1String("ok"));
        }
    }
    emit changed();
    if (!fromToggle)
        return;
    const bool nowUp = !activeTunnel().isEmpty();
    if ((resultKnown && succeeded) || (!resultKnown && nowUp != wasUp)) {
        HardwareOsd::instance()->showVpn(
            isUp(interfaceName), interfaceName, address(interfaceName));
        return;
    }

    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"), 2);
    NotificationService::instance()->Notify(
        QStringLiteral("vpn"), 0, QString(),
        QStringLiteral("VPN toggle failed"),
        QStringLiteral("WireGuard remained %1. Check wg-quick and sudo permissions.")
            .arg(nowUp ? QStringLiteral("connected")
                       : QStringLiteral("disconnected")),
        QStringList(), hints, 0);
}

/* --------------------------------------------------------------- VpnWidget */

VpnWidget::VpnWidget(QWidget *parent) : BarPill(parent)
{
    setFixedHeight(Theme::pillHeight * 2);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    VpnState *vpn = VpnState::instance();
    if (vpn->tunnels().size() == 1) {
        connect(this, &BarPill::clicked, vpn, &VpnState::toggle);
    } else {
        m_popup = new VpnPopup(this);
        connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
        connect(m_popup, &ShellPopup::popupVisibleChanged, this,
                &BarPill::setActive);
    }
    connect(vpn, &VpnState::changed, this, &VpnWidget::sync);
    connect(this, &BarPill::rightClicked, vpn, &VpnState::refresh);
    sync();
}

void VpnWidget::sync()
{
    update();
}

QSize VpnWidget::sizeHint() const
{
    return QSize(230, Theme::pillHeight * 2);
}

void VpnWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    Theme::paintRect(p, rect(),
                     highlighted() ? Theme::surfaceHover : Theme::surface,
                     Theme::radius * 2, Theme::accent, isActive() ? 2 : 0);

    VpnState *vpn = VpnState::instance();
    const QString tunnel = vpn->activeTunnel();
    const bool connected = !tunnel.isEmpty();
    const int iconW = 36;
    p.setFont(Theme::font(28));
    p.setPen(connected ? Theme::accent : Theme::text);
    p.drawText(QRectF(12, 0, iconW, height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               connected ? glyph(U'\U000F099D') : glyph(U'\U000F099E'));

    const int textX = 12 + iconW + 8;
    const int textW = width() - textX - 12;
    if (connected) {
        p.setFont(Theme::font(Theme::smallFontSize, true));
        p.setPen(Theme::textStrong);
        p.drawText(QRectF(textX, 6, textW, 20),
                   Qt::AlignLeft | Qt::AlignVCenter, tunnel);
        p.setFont(Theme::font(Theme::tinyFontSize));
        p.setPen(Theme::textMuted);
        p.drawText(QRectF(textX, height() - 24, textW, 18),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Connected"));
    } else {
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(Theme::textMuted);
        p.drawText(QRectF(textX, 0, textW, height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Disconnected"));
    }
}

/* --------------------------------------------------------------- VpnPopup */

VpnPopup::VpnPopup(QWidget *anchor) : ContentPopup(anchor, 230)
{
    m_manageAsTopLevel = false;
    auto *lay = bodyLayout();
    auto *title = new TextItem(body());
    title->setPixelSize(Theme::bodyFontSize);
    title->setBold(true);
    title->setColor(Theme::textStrong);
    title->setText(QStringLiteral("WireGuard tunnel"));
    lay->addWidget(title);
    lay->addWidget(new HLine(body()));

    for (const QString &tunnel : VpnState::instance()->tunnels()) {
        auto *button = new ShellButton(tunnel, body());
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        lay->addWidget(button);
        m_buttons.push_back(button);
        connect(button, &ShellButton::activated, this, [tunnel]() {
            VpnState::instance()->select(tunnel);
        });
    }
    connect(VpnState::instance(), &VpnState::changed, this, &VpnPopup::sync);
    connect(this, &ShellPopup::popupVisibleChanged, this, [](bool visible) {
        if (visible)
            VpnState::instance()->refresh();
    });
    sync();
}

void VpnPopup::sync()
{
    VpnState *vpn = VpnState::instance();
    const QStringList &tunnels = vpn->tunnels();
    for (int i = 0; i < m_buttons.size() && i < tunnels.size(); ++i) {
        const QString &tunnel = tunnels[i];
        const bool up = vpn->isUp(tunnel);
        m_buttons[i]->setLabel(up ? tunnel + QStringLiteral(" · Connected")
                                  : tunnel);
        m_buttons[i]->setActive(up);
    }
}
