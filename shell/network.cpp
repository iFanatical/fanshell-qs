#include "network.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

#include "status.h"

static QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

static QString sigGlyph(int s)
{
    return s >= 75 ? glyph(U'\U000F0928')
         : s >= 50 ? glyph(U'\U000F0925')
         : s >= 25 ? glyph(U'\U000F0922')
         : s > 0   ? glyph(U'\U000F091F')
                   : glyph(U'\U000F092F');
}

/* ------------------------------------------------------------ NetworkState */

NetworkState *NetworkState::instance()
{
    static NetworkState *s = new NetworkState();
    return s;
}

NetworkState::NetworkState(QObject *parent)
    : QObject(parent), m_watch({"fanshell-qs-net", "watch"})
{
    connect(&m_watch, &BlockWatchProcess::block, this,
            &NetworkState::parseStatus);
    m_watch.start();

    m_wifiList.setCommand({"fanshell-qs-net", "wifi-list"});
    connect(&m_wifiList, &CollectorProcess::finished, this,
            &NetworkState::parseWifi);

    connect(&m_scan, &QProcess::finished, this,
            [this]() { refreshWifi(); });
    connect(&m_action, &QProcess::finished, this,
            [this]() { refreshWifi(); });

    /* Connection changes come from nmcli monitor. Signal strength does not,
     * so sample only those two cheap fields at a low rate instead of running
     * the complete multi-command status collection every five seconds. */
    m_signal.setCommand({"fanshell-qs-net", "signal"});
    connect(&m_signal, &CollectorProcess::finished, this,
            &NetworkState::parseStatus);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, &m_signal,
            [this]() { m_signal.start(); });
    timer->start(30000);
}

void NetworkState::parseStatus(const QString &block)
{
    const QString oldBackend = backend, oldDevice = device,
                  oldGateway = gateway, oldConnectivity = connectivity,
                  oldKind = kind, oldConnName = connName, oldSsid = ssid,
                  oldSecurity = security, oldIp4 = ip4;
    const bool oldWifiEnabled = wifiEnabled;
    const int oldWifiSignal = wifiSignal;
    const QVector<Link> oldLinks = links;

    const QStringList lines = block.trimmed().split('\n');
    QVector<Link> lks;
    bool backendWasReported = false;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("link\t"))) {
            const QStringList p = line.split('\t'); /* link,name,state,type,addr */
            if (p.size() >= 4)
                lks.push_back({p[1], p[2], p[3], p.value(4)});
            continue;
        }
        const int i = line.indexOf('=');
        if (i < 0)
            continue;
        const QString k = line.left(i), v = line.mid(i + 1);
        if (k == QLatin1String("backend")) {
            backend = v;
            backendWasReported = true;
        } else if (k == QLatin1String("device"))
            device = v;
        else if (k == QLatin1String("gateway"))
            gateway = v;
        else if (k == QLatin1String("wifi_enabled"))
            wifiEnabled = (v == QLatin1String("enabled"));
        else if (k == QLatin1String("connectivity"))
            connectivity = v;
        else if (k == QLatin1String("kind"))
            kind = v.isEmpty() ? QStringLiteral("none") : v;
        else if (k == QLatin1String("name"))
            connName = v;
        else if (k == QLatin1String("ssid"))
            ssid = v;
        else if (k == QLatin1String("signal"))
            wifiSignal = v.toInt();
        else if (k == QLatin1String("security"))
            security = v;
        else if (k == QLatin1String("ip"))
            ip4 = v;
    }
    if (backendWasReported && backend == QLatin1String("ip"))
        links = lks;

    auto sameLinks = [](const QVector<Link> &a, const QVector<Link> &b) {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i)
            if (a[i].name != b[i].name || a[i].state != b[i].state
                    || a[i].type != b[i].type || a[i].addr != b[i].addr)
                return false;
        return true;
    };
    if (oldBackend != backend || oldDevice != device || oldGateway != gateway
            || oldWifiEnabled != wifiEnabled
            || oldConnectivity != connectivity || oldKind != kind
            || oldConnName != connName || oldSsid != ssid
            || oldWifiSignal != wifiSignal || oldSecurity != security
            || oldIp4 != ip4 || !sameLinks(oldLinks, links))
        emit changed();
}

void NetworkState::parseWifi(const QString &text)
{
    const bool wasScanning = scanning;
    QVector<Wifi> nets;
    const QStringList lines = text.trimmed().split('\n');
    for (const QString &l : lines) {
        if (l.isEmpty())
            continue;
        const QStringList p = l.split('\t'); /* ssid, signal, security, active */
        Wifi w;
        w.ssid = p.value(0);
        w.signal = p.value(1).toInt();
        w.security = p.value(2);
        w.active = p.value(3) == QLatin1String("1");
        nets.push_back(w);
    }
    bool same = wifiNetworks.size() == nets.size();
    for (int i = 0; same && i < nets.size(); ++i)
        same = wifiNetworks[i].ssid == nets[i].ssid
               && wifiNetworks[i].signal == nets[i].signal
               && wifiNetworks[i].security == nets[i].security
               && wifiNetworks[i].active == nets[i].active;
    wifiNetworks = nets;
    scanning = false;
    if (!same)
        emit wifiListChanged();
    if (!same || wasScanning)
        emit changed();
}

void NetworkState::action(const QStringList &args)
{
    if (m_action.state() != QProcess::NotRunning)
        return;
    m_action.start(QStringLiteral("fanshell-qs-net"), args);
}

void NetworkState::refreshWifi() { m_wifiList.start(); }

void NetworkState::wifiScan()
{
    scanning = true;
    emit changed();
    if (m_scan.state() == QProcess::NotRunning)
        m_scan.start(QStringLiteral("fanshell-qs-net"), {QStringLiteral("wifi-scan")});
}

void NetworkState::wifiConnect(const QString &ssid_, const QString &pw)
{
    if (pw.isEmpty()) {
        action({QStringLiteral("wifi-connect"), ssid_});
        return;
    }
    /* The password goes over stdin, never argv: a command line is readable by
     * every local process through /proc/<pid>/cmdline for as long as the
     * helper (and nmcli under it) runs. */
    if (m_action.state() != QProcess::NotRunning)
        return;
    m_action.start(QStringLiteral("fanshell-qs-net"),
                   {QStringLiteral("wifi-connect"), ssid_,
                    QStringLiteral("--password-stdin")});
    m_action.write(pw.toUtf8() + '\n');
    m_action.closeWriteChannel();
}

void NetworkState::wifiDisconnect() { action({QStringLiteral("wifi-disconnect")}); }
void NetworkState::wifiForget(const QString &s) { action({QStringLiteral("wifi-forget"), s}); }
void NetworkState::setRadio(bool on)
{
    action({QStringLiteral("radio"),
            on ? QStringLiteral("on") : QStringLiteral("off")});
}
void NetworkState::reapply() { action({QStringLiteral("reapply")}); }
void NetworkState::linkUp(const QString &n) { action({QStringLiteral("link-up"), n}); }
void NetworkState::linkDown(const QString &n) { action({QStringLiteral("link-down"), n}); }

/* ----------------------------------------------------------- NetworkWidget */

NetworkWidget::NetworkWidget(QWidget *parent) : BarPill(parent)
{
    m_popup = new NetworkPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);
    connect(this, &BarPill::rightClicked, this, []() {
        if (NetworkState::instance()->isNm())
            NetworkState::instance()->wifiScan();
    });
    connect(NetworkState::instance(), &NetworkState::changed, this,
            &NetworkWidget::sync);
    connect(NetSpeed::instance(), &NetSpeed::changed, this, &NetworkWidget::sync);
    sync();
}

void NetworkWidget::sync()
{
    NetworkState *n = NetworkState::instance();
    QString icon;
    if (n->isNm()) {
        if (n->kind == QLatin1String("wifi"))
            icon = sigGlyph(n->wifiSignal);
        else if (n->kind == QLatin1String("ethernet"))
            icon = glyph(U'\U000F0200');
        else if (!n->wifiEnabled)
            icon = glyph(U'\U000F092D');
        else
            icon = glyph(U'\U000F092E');
    } else {
        icon = n->online() ? glyph(U'\U000F0200') : glyph(U'\U000F092E');
    }
    setIcon(icon);
    setTint(Theme::text);
    /* Padded so the pill keeps its width as the rates change. */
    NetSpeed *speed = NetSpeed::instance();
    setLabel(NetSpeed::enabled() && n->online()
                 ? QStringLiteral("↓ %1  ↑ %2")
                       .arg(NetSpeed::rate(speed->down()).rightJustified(4),
                            NetSpeed::rate(speed->up()).rightJustified(4))
                 : QString());
}

/* ------------------------------------------------------------ list rows -- */

namespace {

/* One Wi-Fi network row: 34px main row + optional 32px password strip. */
class WifiRow : public QWidget {
public:
    WifiRow(const NetworkState::Wifi &w, bool expanded, QWidget *parent)
        : QWidget(parent), m_w(w), m_expanded(expanded),
          m_secured(!w.security.isEmpty() && w.security != QLatin1String("--"))
    {
        setMouseTracking(true);
        setFixedHeight(34 + ((m_expanded && m_secured) ? 32 : 0));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_action = new ShellButton(
            w.active ? QStringLiteral("Disconnect") : QStringLiteral("Connect"),
            this);
        m_action->setDanger(w.active);
        QObject::connect(m_action, &ShellButton::activated, this, [this]() {
            NetworkState *n = NetworkState::instance();
            if (m_w.active) {
                n->wifiDisconnect();
                return;
            }
            if (m_secured) {
                if (onToggleExpand)
                    onToggleExpand(m_w.ssid);
            } else {
                n->wifiConnect(m_w.ssid, QString());
            }
        });

        if (m_expanded && m_secured) {
            m_pw = new QLineEdit(this);
            m_pw->setEchoMode(QLineEdit::Password);
            m_pw->setFrame(false);
            m_pw->setPlaceholderText(QStringLiteral("password"));
            m_pw->setFont(Theme::font(Theme::smallFontSize));
            m_pw->setAttribute(Qt::WA_MacShowFocusRect, false);
            m_pw->setStyleSheet(QStringLiteral(
                "QLineEdit { background: transparent; border: none; color: %1; }")
                                    .arg(Theme::text.name()));
            QPalette pal = m_pw->palette();
            pal.setColor(QPalette::PlaceholderText, Theme::textMuted);
            pal.setColor(QPalette::Highlight, Theme::cyan);
            m_pw->setPalette(pal);
            m_join = new ShellButton(QStringLiteral("Join"), this);
            auto join = [this]() {
                NetworkState::instance()->wifiConnect(m_w.ssid, m_pw->text());
                if (onToggleExpand)
                    onToggleExpand(QString()); /* collapse */
            };
            QObject::connect(m_pw, &QLineEdit::returnPressed, this, join);
            QObject::connect(m_join, &ShellButton::activated, this, join);
        }
    }

    std::function<void(const QString &)> onToggleExpand;

    void focusPassword()
    {
        if (m_pw)
            m_pw->setFocus();
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        const QSize a = m_action->sizeHint();
        m_action->setGeometry(width() - 6 - a.width(), (34 - a.height()) / 2,
                              a.width(), a.height());
        if (m_pw && m_join) {
            const QSize j = m_join->sizeHint();
            const int rowY = 34;
            m_join->setGeometry(width() - 4 - j.width(),
                                rowY + (32 - j.height()) / 2, j.width(),
                                j.height());
            const QFontMetrics fm(m_pw->font());
            const int h = fm.height() + 4;
            m_pw->setGeometry(8, rowY + (32 - h) / 2,
                              m_join->x() - 8 - 8, h);
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        /* main row */
        const QRectF row(0, 0, width(), 34);
        const QColor bg = m_hover ? Theme::surfaceHover
                                  : (m_w.active ? Theme::surface
                                                : Theme::transparent);
        Theme::paintRect(p, row, bg, Theme::smallRadius);

        p.setFont(Theme::font(Theme::bodyFontSize));
        const QString sig = sigGlyph(m_w.signal);
        const int sigW = p.fontMetrics().horizontalAdvance(sig);
        p.setPen(m_w.active ? Theme::accent : Theme::text);
        p.drawText(QRectF(8, 0, sigW, 34), Qt::AlignLeft | Qt::AlignVCenter,
                   sig);

        const int textX = 8 + sigW + 8;
        const int textW = m_action->x() - 8 - textX;
        const QFontMetrics fmS(Theme::font(Theme::smallFontSize));
        const QFontMetrics fmT(Theme::font(Theme::tinyFontSize));
        const int colH = fmS.height() + fmT.height();
        const int colY = int((34 - colH) / 2.0);
        /* lock glyph slot is empty in the current theme -> " " + ssid */
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(Theme::text);
        p.drawText(QRect(textX, colY, textW, fmS.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fmS.elidedText(QStringLiteral(" ") + m_w.ssid,
                                  Qt::ElideRight, textW));
        p.setFont(Theme::font(Theme::tinyFontSize));
        p.setPen(m_w.active ? Theme::success : Theme::textMuted);
        const QString sub = (m_secured ? m_w.security + QStringLiteral(" ")
                                       : QStringLiteral("Open "))
                            + QStringLiteral("· ")
                            + QString::number(m_w.signal)
                            + QStringLiteral("%");
        p.drawText(QRect(textX, colY + fmS.height(), textW, fmT.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, sub);

        /* password strip */
        if (m_expanded && m_secured)
            Theme::paintRect(p, QRectF(0, 34, width(), 32), Theme::surface,
                             Theme::smallRadius, Theme::border, 1);
    }

    void enterEvent(QEnterEvent *) override { updateHover(true); }
    void leaveEvent(QEvent *) override { updateHover(false); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        updateHover(e->position().y() < 34);
    }

private:
    void updateHover(bool h)
    {
        if (m_hover != h) {
            m_hover = h;
            update();
        }
    }

    NetworkState::Wifi m_w;
    bool m_expanded;
    bool m_secured;
    bool m_hover = false;
    ShellButton *m_action;
    QLineEdit *m_pw = nullptr;
    ShellButton *m_join = nullptr;
};

/* One iproute2 link row (bridge/VLAN desktop). */
class LinkRow : public QWidget {
public:
    LinkRow(const NetworkState::Link &l, QWidget *parent)
        : QWidget(parent), m_l(l), m_up(l.state == QLatin1String("UP"))
    {
        setMouseTracking(true);
        setFixedHeight(34);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_btn = new ShellButton(m_up ? QStringLiteral("Down")
                                     : QStringLiteral("Up"),
                                this);
        m_btn->setDanger(m_up);
        QObject::connect(m_btn, &ShellButton::activated, this, [this]() {
            if (m_up)
                NetworkState::instance()->linkDown(m_l.name);
            else
                NetworkState::instance()->linkUp(m_l.name);
        });
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        const QSize b = m_btn->sizeHint();
        m_btn->setGeometry(width() - 6 - b.width(), (34 - b.height()) / 2,
                           b.width(), b.height());
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(),
                         m_hover ? Theme::surfaceHover : Theme::transparent,
                         Theme::smallRadius);
        const int textX = 8;
        const int textW = m_btn->x() - 8 - textX;
        const QFontMetrics fmS(Theme::font(Theme::smallFontSize));
        const QFontMetrics fmT(Theme::font(Theme::tinyFontSize));
        const int colH = fmS.height() + fmT.height();
        const int colY = int((34 - colH) / 2.0);
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(m_up ? Theme::text : Theme::textMuted);
        p.drawText(QRect(textX, colY, textW, fmS.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fmS.elidedText(m_l.name + QStringLiteral("  ") + m_l.addr,
                                  Qt::ElideRight, textW));
        p.setFont(Theme::font(Theme::tinyFontSize));
        p.setPen(m_up ? Theme::success : Theme::textMuted);
        p.drawText(QRect(textX, colY + fmS.height(), textW, fmT.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_l.type + QStringLiteral(" · ") + m_l.state);
    }

    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }

private:
    NetworkState::Link m_l;
    bool m_up;
    bool m_hover = false;
    ShellButton *m_btn;
};

} /* namespace */

/* ------------------------------------------------------------ NetworkPopup */

NetworkPopup::NetworkPopup(QWidget *anchor) : ContentPopup(anchor, 360, 560)
{
    auto *lay = bodyLayout();

    /* ---- header ---- */
    auto *header = new QWidget(body());
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(Theme::rowSpacing);
    m_headerTitle = new TextItem(header);
    m_headerTitle->setPixelSize(Theme::bodyFontSize);
    m_headerTitle->setBold(true);
    m_headerTitle->setColor(Theme::textStrong);
    hl->addWidget(m_headerTitle, 0, Qt::AlignVCenter);
    hl->addStretch(1);
    m_radioBtn = new ShellButton(QString(), header);
    m_scanBtn = new ShellButton(QString(), header);
    m_reapplyBtn = new ShellButton(QStringLiteral("Re-apply"), header);
    hl->addWidget(m_radioBtn, 0, Qt::AlignVCenter);
    hl->addWidget(m_scanBtn, 0, Qt::AlignVCenter);
    hl->addWidget(m_reapplyBtn, 0, Qt::AlignVCenter);
    lay->addWidget(header);

    connect(m_radioBtn, &ShellButton::activated, this, []() {
        NetworkState *n = NetworkState::instance();
        n->setRadio(!n->wifiEnabled);
    });
    connect(m_scanBtn, &ShellButton::activated, this,
            []() { NetworkState::instance()->wifiScan(); });
    connect(m_reapplyBtn, &ShellButton::activated, this,
            []() { NetworkState::instance()->reapply(); });

    lay->addWidget(new HLine(body()));

    /* ---- active-connection summary ---- */
    auto *summary = new QWidget(body());
    auto *sl = new QVBoxLayout(summary);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(1);
    m_summary = new TextItem(summary);
    m_summary->setPixelSize(Theme::smallFontSize);
    m_summary->setBold(true);
    m_summarySub = new TextItem(summary);
    m_summarySub->setPixelSize(Theme::tinyFontSize);
    m_summarySub->setColor(Theme::textMuted);
    sl->addWidget(m_summary);
    sl->addWidget(m_summarySub);
    lay->addWidget(summary);

    /* ---- network / link list ---- */
    m_listBox = new QWidget(body());
    auto *ll = new QVBoxLayout(m_listBox);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(Theme::popupSpacing);
    lay->addWidget(m_listBox);
    lay->addStretch(0);

    connect(NetworkState::instance(), &NetworkState::changed, this,
            &NetworkPopup::syncControls);
    connect(NetworkState::instance(), &NetworkState::wifiListChanged, this,
            &NetworkPopup::rebuildList);
    syncControls();
    rebuildList();
}

void NetworkPopup::openPopup()
{
    if (NetworkState::instance()->isNm())
        NetworkState::instance()->refreshWifi();
    ContentPopup::openPopup();
}

void NetworkPopup::syncControls()
{
    NetworkState *n = NetworkState::instance();
    const bool nm = n->isNm();

    m_headerTitle->setText(nm ? QStringLiteral("Network")
                              : QStringLiteral("Network (wired)"));
    m_radioBtn->setVisible(nm);
    m_radioBtn->setLabel(n->wifiEnabled ? QStringLiteral("Wi-Fi On")
                                        : QStringLiteral("Wi-Fi Off"));
    m_radioBtn->setActive(n->wifiEnabled);
    m_scanBtn->setVisible(nm);
    m_scanBtn->setLabel(n->scanning ? QStringLiteral("Scanning…")
                                    : QStringLiteral("Scan"));
    m_scanBtn->setEnabled(n->wifiEnabled);
    m_reapplyBtn->setVisible(!nm);

    QString line1;
    if (nm) {
        if (n->kind == QLatin1String("wifi"))
            line1 = QStringLiteral(" ")
                    + (n->ssid.isEmpty() ? QStringLiteral("—") : n->ssid);
        else if (n->kind == QLatin1String("ethernet"))
            line1 = glyph(U'\U000F0200') + QStringLiteral(" ")
                    + (n->connName.isEmpty() ? QStringLiteral("Wired")
                                             : n->connName);
        else
            line1 = glyph(U'\U000F092E') + QStringLiteral(" Disconnected");
    } else {
        line1 = glyph(U'\U000F0200') + QStringLiteral(" ")
                + (n->device.isEmpty() ? QStringLiteral("—") : n->device);
    }
    m_summary->setText(line1);
    m_summary->setColor(n->online() ? Theme::success : Theme::text);
    m_summarySub->setText(
        (!n->ip4.isEmpty() || !n->gateway.isEmpty())
            ? QStringLiteral("IP ")
                  + (n->ip4.isEmpty() ? QStringLiteral("—") : n->ip4)
                  + QStringLiteral("   GW ")
                  + (n->gateway.isEmpty() ? QStringLiteral("—")
                                          : n->gateway)
            : QStringLiteral("no address"));

    /* Rebuild the row list only when its inputs actually changed. */
    QStringList linksKey;
    for (const NetworkState::Link &l : n->links)
        linksKey << l.name + QLatin1Char('|') + l.state + QLatin1Char('|')
                        + l.type + QLatin1Char('|') + l.addr;
    if (nm != m_lastNm || (!nm && linksKey != m_lastLinksKey)) {
        m_lastNm = nm;
        m_lastLinksKey = linksKey;
        rebuildList();
        return;
    }
    m_lastLinksKey = linksKey;

    if (isVisible())
        relayout();
}

void NetworkPopup::rebuildList()
{
    NetworkState *n = NetworkState::instance();
    const bool nm = n->isNm();

    auto *ll = static_cast<QVBoxLayout *>(m_listBox->layout());
    while (QLayoutItem *it = ll->takeAt(0)) {
        if (it->widget()) {
            it->widget()->hide(); /* deleteLater leaves it painted a tick */
            it->widget()->deleteLater();
        }
        delete it;
    }

    if (nm) {
        WifiRow *expanded = nullptr;
        for (const NetworkState::Wifi &w : n->wifiNetworks) {
            auto *row = new WifiRow(w, m_selectedSsid == w.ssid, m_listBox);
            row->onToggleExpand = [this](const QString &ssid) {
                m_selectedSsid = (m_selectedSsid == ssid) ? QString() : ssid;
                rebuildList();
            };
            ll->addWidget(row);
            row->show();
            if (m_selectedSsid == w.ssid)
                expanded = row;
        }
        if (expanded)
            expanded->focusPassword();
        if (n->wifiNetworks.isEmpty()) {
            auto *empty = new TextItem(m_listBox);
            empty->setPixelSize(Theme::smallFontSize);
            empty->setColor(Theme::textMuted);
            empty->setText(n->wifiEnabled
                               ? QStringLiteral("No networks — hit Scan.")
                               : QStringLiteral("Wi-Fi is off."));
            ll->addWidget(empty);
            empty->show();
        }
    } else {
        for (const NetworkState::Link &l : n->links) {
            auto *row = new LinkRow(l, m_listBox);
            ll->addWidget(row);
            row->show();
        }
    }

    if (isVisible())
        relayout();
}
