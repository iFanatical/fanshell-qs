#include "battery.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QSocketNotifier>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include "paths.h"
#include "powerprofiles.h"

static QString formatDuration(int minutes)
{
    if (minutes < 0)
        return QString();
    const int h = minutes / 60, m = minutes % 60;
    if (h > 0)
        return QStringLiteral("%1h %2m").arg(h).arg(m);
    return QStringLiteral("%1m").arg(m);
}

/* ----------------------------------------------------------- BatterySaver */

namespace {

/* Switches the power profile to Power Saver when the battery, draining,
 * falls to power.saver_below percent (default 50; off turns it off), and
 * back to the profile it found once the charger is in. Only on crossing:
 * a profile picked by hand in between stays, and the one found is put back
 * only if Power Saver is still the profile then. */
class BatterySaver : public QObject {
public:
    explicit BatterySaver(BatteryState *battery)
        : QObject(battery), m_battery(battery)
    {
        connect(battery, &BatteryState::changed, this, &BatterySaver::check);
        /* At login the profile daemon may answer after the battery. */
        connect(PowerProfileState::instance(), &PowerProfileState::changed,
                this, &BatterySaver::check);
    }

private:
    static int threshold()
    {
        const QString v = Paths::setting(QStringLiteral("power.saver_below"),
                                         QStringLiteral("50"));
        return v.toLower() == QLatin1String("off") ? 0 : v.toInt();
    }

    void check()
    {
        PowerProfileState *power = PowerProfileState::instance();
        if (!power->available())
            return;
        const bool low = m_battery->present
                         && m_battery->status == QLatin1String("Discharging")
                         && m_battery->percent <= threshold();
        if (low == m_low)
            return;
        m_low = low;
        const QString saver = QStringLiteral("power-saver");
        if (low) {
            if (power->activeProfile() != saver
                    && power->profiles().contains(saver)) {
                m_before = power->activeProfile();
                power->setProfile(saver);
            }
        } else if (!m_before.isEmpty()) {
            if (power->activeProfile() == saver)
                power->setProfile(m_before);
            m_before.clear();
        }
    }

    BatteryState *m_battery;
    bool m_low = false;
    QString m_before; /* the profile it found, while it has switched */
};

} /* namespace */

/* ----------------------------------------------------------- BatteryState */

BatteryState *BatteryState::instance()
{
    static BatteryState *s = new BatteryState();
    return s;
}

BatteryState::BatteryState(QObject *parent) : QObject(parent)
{
    m_proc.setCommand({"fanshell-qs-battery"});
    connect(&m_proc, &CollectorProcess::finished, this, &BatteryState::parse);

    /* Plugging in and unplugging come as uevents (watchUevents), so the poll
     * only follows the charge, which moves about a percent a minute at most. */
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, &m_proc, [this]() { m_proc.start(); });
    timer->start(60000);
    m_proc.start();
    watchUevents();
    new BatterySaver(this);
}

/* The kernel's uevents for power supplies: the charger going in or out (the
 * AC supply), and each battery's own status changes. A plug comes as a burst
 * of them, AC first; the battery's status follows a moment later on some
 * firmware (the HP's says Discharging for a second or two after the plug),
 * so read now and once more after it has settled. */
void BatteryState::watchUevents()
{
    const int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          NETLINK_KOBJECT_UEVENT);
    if (fd < 0)
        return;
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1; /* the kernel's own events; udev's are group 2 */
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return;
    }

    auto *burst = new QTimer(this);
    burst->setSingleShot(true);
    burst->setInterval(100);
    connect(burst, &QTimer::timeout, &m_proc, [this]() { m_proc.start(); });
    auto *settle = new QTimer(this);
    settle->setSingleShot(true);
    settle->setInterval(2500);
    connect(settle, &QTimer::timeout, &m_proc, [this]() { m_proc.start(); });

    auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [fd, burst, settle]() {
        static const char key[] = "SUBSYSTEM=power_supply"; /* with its NUL */
        char buf[8192];
        bool power = false;
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
            if (memmem(buf, size_t(n), key, sizeof(key)))
                power = true;
        }
        if (power) {
            burst->start();
            settle->start();
        }
    });
}

void BatteryState::parse(const QString &text)
{
    const QString trimmed = text.trimmed();
    present = !trimmed.isEmpty();
    if (!present) {
        emit changed();
        return;
    }

    const QStringList lines = trimmed.split('\n');
    for (const QString &line : lines) {
        const int i = line.indexOf('=');
        if (i < 0)
            continue;
        const QString k = line.left(i), v = line.mid(i + 1);
        if (k == QLatin1String("percent"))
            percent = v.toInt();
        else if (k == QLatin1String("status"))
            status = v;
        else if (k == QLatin1String("minutes"))
            minutes = v.isEmpty() ? -1 : v.toInt();
        else if (k == QLatin1String("icon"))
            icon = v;
        else if (k == QLatin1String("color"))
            color = v.isEmpty() ? Theme::text : QColor(v);
    }
    emit changed();
}

QString BatteryState::summary() const
{
    if (status == QLatin1String("Full"))
        return QStringLiteral("Fully charged");
    const QString dur = formatDuration(minutes);
    if (status == QLatin1String("Charging"))
        return dur.isEmpty() ? status
                              : QStringLiteral("Charging — %1 until full")
                                    .arg(dur);
    if (status == QLatin1String("Discharging"))
        return dur.isEmpty() ? status
                              : QStringLiteral("%1 remaining").arg(dur);
    return status.isEmpty() ? QStringLiteral("Unknown") : status;
}

/* ---------------------------------------------------------- BatteryWidget */

BatteryWidget::BatteryWidget(QWidget *parent) : BarPill(parent)
{
    setBold(true);
    m_popup = new BatteryPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);
    connect(BatteryState::instance(), &BatteryState::changed, this,
            &BatteryWidget::sync);
    sync();
}

void BatteryWidget::sync()
{
    BatteryState *b = BatteryState::instance();
    setVisible(b->present);
    if (!b->present)
        return;
    setIcon(b->icon);
    setLabel(QString::number(b->percent) + QStringLiteral("%"));
    setTint(b->color);
    setToolTip(QString::number(b->percent) + QStringLiteral("% — ")
              + b->summary());
}

/* ---------------------------------------------------------------- BatteryBar */

/* Declared in battery.h (as `class BatteryBar *m_bar`), so it must live at
 * namespace scope, not anonymous -- unlike the other file-local row/section
 * widgets in this codebase, which are never named outside their own .cpp. */
class BatteryBar : public QWidget {
public:
    explicit BatteryBar(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setValue(qreal v) { m_v = v; update(); }
    void setColor(const QColor &c) { m_color = c; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(), Theme::surface, 4);
        const qreal v = qBound<qreal>(0, m_v, 1);
        if (v > 0)
            Theme::paintRect(p, QRectF(0, 0, width() * v, height()), m_color,
                             4);
    }

private:
    qreal m_v = 0;
    QColor m_color = Theme::accent;
};

/* ----------------------------------------------------------- BatteryPopup */

BatteryPopup::BatteryPopup(QWidget *anchor) : ContentPopup(anchor, 260)
{
    auto *lay = bodyLayout();

    auto *title = new TextItem(body());
    title->setPixelSize(Theme::bodyFontSize);
    title->setBold(true);
    title->setColor(Theme::textStrong);
    title->setText(QStringLiteral("Battery"));
    lay->addWidget(title);

    lay->addWidget(new HLine(body()));

    auto *pctRow = new QWidget(body());
    auto *pl = new QHBoxLayout(pctRow);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(Theme::rowSpacing);
    m_icon = new TextItem(pctRow);
    m_icon->setPixelSize(Theme::titleFontSize);
    m_icon->setColor(Theme::textStrong);
    m_percentText = new TextItem(pctRow);
    m_percentText->setPixelSize(Theme::titleFontSize);
    m_percentText->setBold(true);
    m_percentText->setColor(Theme::textStrong);
    pl->addWidget(m_icon, 0, Qt::AlignVCenter);
    pl->addWidget(m_percentText, 0, Qt::AlignVCenter);
    pl->addStretch(1);
    lay->addWidget(pctRow);

    m_bar = new BatteryBar(body());
    lay->addWidget(m_bar);

    m_statusText = new TextItem(body());
    m_statusText->setPixelSize(Theme::smallFontSize);
    m_statusText->setColor(Theme::textMuted);
    lay->addWidget(m_statusText);

    lay->addStretch(0);

    connect(BatteryState::instance(), &BatteryState::changed, this,
            &BatteryPopup::sync);
    sync();
}

void BatteryPopup::sync()
{
    BatteryState *b = BatteryState::instance();
    m_icon->setText(b->icon);
    m_percentText->setText(QString::number(b->percent) + QStringLiteral("%"));
    m_statusText->setText(b->summary());
    m_bar->setValue(b->percent / 100.0);
    m_bar->setColor(b->color.isValid() ? b->color : Theme::accent);
    if (isVisible())
        relayout();
}

/* ------------------------------------------------------------ PowerDrawState */

PowerDrawState *PowerDrawState::instance()
{
    static PowerDrawState *s = new PowerDrawState();
    return s;
}

PowerDrawState::PowerDrawState(QObject *parent) : QObject(parent)
{
    m_proc.setCommand({"fanshell-qs-powerdraw"});
    connect(&m_proc, &CollectorProcess::finished, this, &PowerDrawState::parse);
    m_ups.setCommand({"fanshell-qs-ups"});
    connect(&m_ups, &CollectorProcess::finished, this,
            &PowerDrawState::parseUps);

    /* Slower than the battery's 15s: this is ambient interest, and the
     * component script holds a sample window open to difference RAPL. */
    auto *timer = new QTimer(this);
    timer->setInterval(20000);
    connect(timer, &QTimer::timeout, this, [this]() {
        m_proc.start();
        m_ups.start();
    });

    /* The power pill only ever shows on a machine without a battery, so a
     * laptop never needs these samples. Wait for BatteryState's first answer
     * and poll only while it says there is no battery. */
    connect(BatteryState::instance(), &BatteryState::changed, this,
            [this, timer]() {
                const bool wanted = !BatteryState::instance()->present;
                if (wanted && !timer->isActive()) {
                    timer->start();
                    m_proc.start();
                    m_ups.start();
                } else if (!wanted && timer->isActive()) {
                    timer->stop();
                }
            });
}

void PowerDrawState::parse(const QString &text)
{
    cpu = -1;
    gpu = -1;
    cpuLocked = false;
    for (const QString &line : text.trimmed().split('\n')) {
        const int i = line.indexOf('=');
        if (i < 0)
            continue;
        const QString k = line.left(i);
        const QString v = line.mid(i + 1).trimmed();
        if (k == QLatin1String("cpu"))
            cpu = v.toDouble();
        else if (k == QLatin1String("gpu"))
            gpu = v.toDouble();
        else if (k == QLatin1String("cpu_locked"))
            cpuLocked = (v == QLatin1String("1"));
    }
    emit changed();
}

void PowerDrawState::parseUps(const QString &text)
{
    /* No output means no UPS configured, or the card stopped answering --
     * either way fall back to the component estimate rather than freezing on
     * a stale reading. */
    const QString trimmed = text.trimmed();
    upsPresent = !trimmed.isEmpty();
    if (!upsPresent) {
        emit changed();
        return;
    }
    for (const QString &line : trimmed.split('\n')) {
        const int i = line.indexOf('=');
        if (i < 0)
            continue;
        const QString k = line.left(i);
        const QString v = line.mid(i + 1).trimmed();
        if (k == QLatin1String("watts"))
            upsWatts = v.toInt();
        else if (k == QLatin1String("load"))
            upsLoad = v.toInt();
        else if (k == QLatin1String("rated"))
            upsRated = v.toInt();
        else if (k == QLatin1String("minutes"))
            upsMinutes = v.toInt();
        else if (k == QLatin1String("charge"))
            upsCharge = v.toInt();
        else if (k == QLatin1String("state"))
            upsState = v;
        else if (k == QLatin1String("onbattery"))
            onBattery = (v == QLatin1String("1"));
        else if (k == QLatin1String("input"))
            upsInput = v.toDouble();
    }
    emit changed();
}

/* Headroom colour. The point of showing total draw is spotting when the UPS
 * is being pushed, so the pill changes colour before the limit rather than
 * at it. */
static QColor loadTint(int loadPercent)
{
    if (loadPercent >= 85)
        return Theme::danger;
    if (loadPercent >= 70)
        return Theme::warning;
    return Theme::text;
}

/* ----------------------------------------------------------- PowerDrawWidget */

PowerDrawWidget::PowerDrawWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(QStringLiteral("\U000F06A5")); /* mdi power-plug */
    m_popup = new PowerDrawPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);
    connect(PowerDrawState::instance(), &PowerDrawState::changed, this,
            &PowerDrawWidget::sync);
    connect(BatteryState::instance(), &BatteryState::changed, this,
            &PowerDrawWidget::sync);
    sync();
}

void PowerDrawWidget::sync()
{
    PowerDrawState *p = PowerDrawState::instance();
    /* Only on a machine with no battery, and only if something is readable --
     * a plug icon reporting nothing would be pure decoration. */
    const bool show = !BatteryState::instance()->present
                      && (p->upsPresent || p->any());
    setVisible(show);
    if (!show)
        return;

    if (p->upsPresent) {
        /* Real wall power for everything on the UPS. */
        setIcon(p->onBattery ? QStringLiteral("\U000F0084")  /* battery-alert */
                             : QStringLiteral("\U000F06A5"));
        setLabel(QString::number(p->upsWatts) + QStringLiteral(" W"));
        setTint(p->onBattery ? Theme::danger : loadTint(p->upsLoad));
        QString tip = QStringLiteral("UPS %1 W of %2 W (%3%)")
                          .arg(p->upsWatts)
                          .arg(p->upsRated)
                          .arg(p->upsLoad);
        if (p->upsMinutes >= 0)
            tip += QStringLiteral(" · %1 min runtime").arg(p->upsMinutes);
        if (p->onBattery)
            tip += QStringLiteral(" · ON BATTERY");
        setToolTip(tip);
        return;
    }

    /* No UPS: the CPU+GPU estimate, which is components only. */
    setIcon(QStringLiteral("\U000F06A5"));
    const qreal total = qMax<qreal>(0, p->cpu) + qMax<qreal>(0, p->gpu);
    setLabel(QString::number(total, 'f', 0) + QStringLiteral(" W"));
    setTint(Theme::text);
    QStringList parts;
    if (p->cpu >= 0)
        parts << QStringLiteral("CPU %1 W").arg(p->cpu, 0, 'f', 1);
    if (p->gpu >= 0)
        parts << QStringLiteral("GPU %1 W").arg(p->gpu, 0, 'f', 1);
    setToolTip(parts.join(QStringLiteral(" · ")));
}

/* ------------------------------------------------------------ PowerDrawPopup */

PowerDrawPopup::PowerDrawPopup(QWidget *anchor) : ContentPopup(anchor, 300)
{
    auto *title = new TextItem(body());
    title->setText(QStringLiteral("Power draw"));
    title->setPixelSize(Theme::bodyFontSize);
    title->setBold(true);
    title->setColor(Theme::accent);
    bodyLayout()->addWidget(title);

    /* --- UPS group --- */
    m_upsLoad = new TextItem(body());
    m_upsLoad->setColor(Theme::text);
    bodyLayout()->addWidget(m_upsLoad);

    m_upsBar = new BatteryBar(body());
    bodyLayout()->addWidget(m_upsBar);

    m_upsRuntime = new TextItem(body());
    m_upsRuntime->setColor(Theme::text);
    bodyLayout()->addWidget(m_upsRuntime);

    m_upsBattery = new TextItem(body());
    m_upsBattery->setColor(Theme::text);
    bodyLayout()->addWidget(m_upsBattery);

    m_upsInput = new TextItem(body());
    m_upsInput->setColor(Theme::textMuted);
    m_upsInput->setPixelSize(Theme::smallFontSize);
    bodyLayout()->addWidget(m_upsInput);

    /* --- component group --- */
    m_cpu = new TextItem(body());
    m_cpu->setColor(Theme::text);
    bodyLayout()->addWidget(m_cpu);

    m_gpu = new TextItem(body());
    m_gpu->setColor(Theme::text);
    bodyLayout()->addWidget(m_gpu);

    m_note1 = new TextItem(body());
    m_note1->setColor(Theme::textMuted);
    m_note1->setPixelSize(Theme::tinyFontSize);
    bodyLayout()->addWidget(m_note1);

    m_note2 = new TextItem(body());
    m_note2->setColor(Theme::textMuted);
    m_note2->setPixelSize(Theme::tinyFontSize);
    bodyLayout()->addWidget(m_note2);

    connect(PowerDrawState::instance(), &PowerDrawState::changed, this,
            &PowerDrawPopup::sync);
    sync();
}

void PowerDrawPopup::sync()
{
    PowerDrawState *p = PowerDrawState::instance();
    const bool ups = p->upsPresent;

    m_upsLoad->setVisible(ups);
    m_upsBar->setVisible(ups);
    m_upsRuntime->setVisible(ups);
    m_upsBattery->setVisible(ups);
    m_upsInput->setVisible(ups);

    if (ups) {
        m_upsLoad->setText(QStringLiteral("%1 W of %2 W  ·  %3%")
                               .arg(p->upsWatts)
                               .arg(p->upsRated)
                               .arg(p->upsLoad));
        m_upsBar->setValue(p->upsRated > 0
                               ? qreal(p->upsWatts) / p->upsRated
                               : 0);
        m_upsBar->setColor(p->onBattery ? Theme::danger
                                        : loadTint(p->upsLoad));
        m_upsRuntime->setText(
            p->upsMinutes >= 0
                ? QStringLiteral("Runtime      %1 min").arg(p->upsMinutes)
                : QStringLiteral("Runtime      unknown"));
        m_upsBattery->setText(QStringLiteral("Battery      %1%  ·  %2")
                                  .arg(p->upsCharge)
                                  .arg(p->upsState));
        m_upsInput->setText(QStringLiteral("Input %1 V")
                                .arg(p->upsInput, 0, 'f', 1));
    }

    /* With a UPS the component figures still earn their place: they say how
     * much of the wall total is this machine rather than everything else
     * sharing the UPS. Without one they are the whole story. */
    const bool haveCpu = p->cpu >= 0;
    const bool haveGpu = p->gpu >= 0;
    m_cpu->setVisible(haveCpu || !ups);
    m_gpu->setVisible(haveGpu || !ups);
    m_cpu->setColor(ups ? Theme::textMuted : Theme::text);
    m_gpu->setColor(ups ? Theme::textMuted : Theme::text);
    m_cpu->setText(haveCpu ? QStringLiteral("CPU package   %1 W")
                                 .arg(p->cpu, 0, 'f', 1)
                           : QStringLiteral("CPU package   unavailable"));
    m_gpu->setText(haveGpu ? QStringLiteral("GPU board     %1 W")
                                 .arg(p->gpu, 0, 'f', 1)
                           : QStringLiteral("GPU board     unavailable"));

    /* Say what the number is not. */
    if (p->cpuLocked) {
        m_note1->setText(QStringLiteral("CPU counter is root-only."));
        m_note2->setText(QStringLiteral("Run tools/install-rapl-udev.sh"));
        m_note1->setVisible(true);
        m_note2->setVisible(true);
    } else if (ups) {
        m_note1->setText(QStringLiteral("Wall power for everything on the UPS."));
        m_note2->setVisible(false);
        m_note1->setVisible(true);
    } else {
        m_note1->setText(QStringLiteral("Components only — not wall power."));
        m_note2->setText(
            QStringLiteral("Excludes RAM, drives, fans, PSU loss."));
        m_note1->setVisible(true);
        m_note2->setVisible(true);
    }
    relayout();
}
