#include "status.h"

#include <QFile>
#include <QProcess>
#include <QStorageInfo>

#include <algorithm>
#include <csignal>

#include "gamemode.h"
#include "idleinhibit.h"
#include "paths.h"

namespace {

QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

} /* namespace */

/* ------------------------------------------------------------ UpdatesState */

UpdatesState *UpdatesState::instance()
{
    static UpdatesState *s = new UpdatesState();
    return s;
}

UpdatesState::UpdatesState(QObject *parent) : QObject(parent)
{
    m_proc.setCommand({QStringLiteral("fanshell-qs-updates")});
    connect(&m_proc, &CollectorProcess::finished, this, &UpdatesState::parse);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        check();
        bool ok = false;
        const double hours =
            Paths::setting(QStringLiteral("updates.interval")).toDouble(&ok);
        m_timer.start(int(qBound(0.25, ok ? hours : 6.0, 168.0) * 3600 * 1000));
    });
    /* Not at login, when the session is busiest. */
    m_timer.setSingleShot(true);
    m_timer.start(60 * 1000);
}

bool UpdatesState::enabled() const
{
    return Paths::setting(QStringLiteral("updates")).toLower() != QLatin1String("off");
}

void UpdatesState::check()
{
    if (enabled())
        m_proc.start();
}

void UpdatesState::parse(const QString &text)
{
    int count = -1;
    for (const QString &line : text.split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("count=")))
            count = line.mid(6).toInt();
    if (count == m_count)
        return;
    m_count = count;
    emit changed();
}

void UpdatesState::upgrade()
{
    if (m_upgrade)
        return;
    m_upgrade = new QProcess(this);
    m_upgrade->setProgram(QStringLiteral("fanshell-qs-menu-action"));
    m_upgrade->setArguments({QStringLiteral("execute:update")});
    m_upgrade->setChildProcessModifier([]() {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
    });
    auto done = [this]() {
        m_upgrade->deleteLater();
        m_upgrade = nullptr;
        check();
    };
    connect(m_upgrade, &QProcess::finished, this, done);
    connect(m_upgrade, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done();
    });
    m_upgrade->start();
}

/* --------------------------------------------------------------- DiskState */

DiskState *DiskState::instance()
{
    static DiskState *s = new DiskState();
    return s;
}

DiskState::DiskState(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(5 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &DiskState::refresh);
    m_timer.start();
    refresh();
}

int DiskState::warnAt()
{
    bool ok = false;
    const int pct = Paths::setting(QStringLiteral("disk.warn")).toInt(&ok);
    return ok ? qBound(1, pct, 100) : 90;
}

void DiskState::refresh()
{
    QVector<Mount> mounts;
    QList<QByteArray> devices;
    for (const QString &path : {QStringLiteral("/"), QStringLiteral("/home")}) {
        QStorageInfo info(path);
        if (!info.isValid() || !info.isReady() || info.bytesTotal() <= 0
                || devices.contains(info.device()))
            continue;
        devices << info.device();
        Mount m;
        m.path = info.rootPath();
        m.freeBytes = info.bytesAvailable();
        m.percent = int(100 - 100 * info.bytesAvailable() / info.bytesTotal());
        mounts << m;
    }
    bool same = mounts.size() == m_mounts.size();
    for (int i = 0; same && i < mounts.size(); i++)
        same = mounts[i].path == m_mounts[i].path
               && mounts[i].percent == m_mounts[i].percent;
    m_mounts = mounts;
    if (!same)
        emit changed();
}

DiskState::Mount DiskState::fullest() const
{
    Mount best;
    best.percent = -1;
    for (const Mount &m : m_mounts)
        if (m.percent > best.percent)
            best = m;
    return best;
}

/* ---------------------------------------------------------------- NetSpeed */

NetSpeed *NetSpeed::instance()
{
    static NetSpeed *s = new NetSpeed();
    return s;
}

NetSpeed::NetSpeed(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &NetSpeed::sample);
    m_timer.start();
    sample();
}

bool NetSpeed::enabled()
{
    return Paths::setting(QStringLiteral("network.speed")).toLower() != QLatin1String("off");
}

QString NetSpeed::rate(double bps)
{
    static const char *units[] = {"B", "K", "M", "G"};
    int u = 0;
    while (bps >= 1000 && u < 3) {
        bps /= 1024;
        u++;
    }
    return (bps < 10 && u > 0 ? QString::number(bps, 'f', 1)
                              : QString::number(qRound(bps)))
           + QLatin1String(units[u]);
}

void NetSpeed::sample()
{
    QFile f(QStringLiteral("/proc/net/dev"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    static const QStringList skip{
        QStringLiteral("lo"), QStringLiteral("tun"), QStringLiteral("wg"),
        QStringLiteral("docker"), QStringLiteral("veth"), QStringLiteral("virbr"),
        QStringLiteral("br-"), QStringLiteral("vnet"), QStringLiteral("tap")};
    quint64 rx = 0, tx = 0;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (int i = 2; i < lines.size(); i++) {
        const int colon = lines[i].indexOf(':');
        if (colon < 0)
            continue;
        const QString name = QString::fromLatin1(lines[i].left(colon).trimmed());
        if (std::any_of(skip.cbegin(), skip.cend(),
                        [&name](const QString &p) { return name.startsWith(p); }))
            continue;
        const QList<QByteArray> cols = lines[i].mid(colon + 1).simplified().split(' ');
        if (cols.size() < 9)
            continue;
        rx += cols[0].toULongLong();
        tx += cols[8].toULongLong();
    }
    const qint64 ms = m_clock.restart();
    if (m_primed && ms > 0 && rx >= m_rx && tx >= m_tx) {
        m_down = (rx - m_rx) * 1000.0 / ms;
        m_up = (tx - m_tx) * 1000.0 / ms;
        emit changed();
    }
    m_rx = rx;
    m_tx = tx;
    m_primed = true;
}

/* ----------------------------------------------------------- UpdatesWidget */

UpdatesWidget::UpdatesWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(glyph(U'\U000F03D5')); /* mdi package-up */
    setTint(Theme::accent);
    hide();
    UpdatesState *u = UpdatesState::instance();
    connect(this, &BarPill::clicked, u, &UpdatesState::upgrade);
    connect(this, &BarPill::rightClicked, u, &UpdatesState::check);
    connect(u, &UpdatesState::changed, this, &UpdatesWidget::sync);
    sync();
}

void UpdatesWidget::sync()
{
    const int n = UpdatesState::instance()->count();
    setLabel(QString::number(n));
    setToolTip(QStringLiteral("%1 update%2 waiting. Click to upgrade; right-click "
                              "to check again")
                   .arg(n)
                   .arg(n == 1 ? "" : "s"));
    setVisible(n > 0 && UpdatesState::instance()->enabled());
}

/* -------------------------------------------------------------- DiskWidget */

DiskWidget::DiskWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(glyph(U'\U000F02CA')); /* mdi harddisk */
    hide();
    connect(DiskState::instance(), &DiskState::changed, this, &DiskWidget::sync);
    connect(this, &BarPill::clicked, DiskState::instance(), &DiskState::refresh);
    sync();
}

void DiskWidget::sync()
{
    const DiskState::Mount m = DiskState::instance()->fullest();
    const bool show = m.percent >= DiskState::warnAt();
    setVisible(show);
    if (!show)
        return;
    setLabel(QStringLiteral("%1%").arg(m.percent));
    setTint(m.percent >= 97 ? Theme::danger : Theme::warning);
    setToolTip(QStringLiteral("%1 is %2% full, %3 free")
                   .arg(m.path)
                   .arg(m.percent)
                   .arg(NetSpeed::rate(double(m.freeBytes))));
}

/* ------------------------------------------------------------- ModesWidget */

ModesWidget::ModesWidget(QWidget *parent) : BarPill(parent)
{
    setTint(Theme::warning);
    hide();
    connect(this, &BarPill::clicked, this, []() {
        IdleInhibitor::instance()->setKeepAwake(false);
        GameMode::instance()->setActive(false);
    });
    connect(IdleInhibitor::instance(), &IdleInhibitor::changed, this,
            &ModesWidget::sync);
    connect(GameMode::instance(), &GameMode::changed, this, &ModesWidget::sync);
    sync();
}

void ModesWidget::sync()
{
    const bool awake = IdleInhibitor::instance()->keepAwake();
    const bool game = GameMode::instance()->active();
    QStringList icons, names;
    if (game) {
        icons << glyph(U'\U000F0297'); /* mdi gamepad-variant */
        names << QStringLiteral("game mode");
    }
    if (awake) {
        icons << glyph(U'\U000F0176'); /* mdi coffee */
        names << QStringLiteral("keep awake");
    }
    setIcon(icons.join(QLatin1Char(' ')));
    setToolTip(names.isEmpty() ? QString()
                               : QStringLiteral("On: %1. Click to turn off")
                                     .arg(names.join(QStringLiteral(", "))));
    setVisible(!icons.isEmpty());
}
