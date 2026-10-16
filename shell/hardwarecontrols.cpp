#include "hardwarecontrols.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

#include "notification.h"
#include "osd.h"
#include "volume.h"

namespace {

constexpr int audioStep = 5;
/* brightnessctl answers instantly; ddcutil over a slow I2C bus can take a
 * few seconds. Anything past this is wedged, and must not leave the busy
 * flag set forever with every later key press silently queued behind it. */
constexpr int helperTimeoutMs = 15000;

int percentFromBrightnessctl(const QByteArray &output)
{
    const QStringList lines = QString::fromLocal8Bit(output).split(
        QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(','));
        if (fields.size() < 4)
            continue;
        QString value = fields[3].trimmed();
        value.remove(QLatin1Char('%'));
        bool ok = false;
        const int percent = value.toInt(&ok);
        if (ok)
            return qBound(0, percent, 100);
    }
    return -1;
}

QString eventSound()
{
    static const QString sound = []() {
        QStringList roots = QStandardPaths::standardLocations(
            QStandardPaths::ConfigLocation);
        /* fanshell-qs/sounds first; hypr/sounds still holds the set from the
         * Hyprland configuration on machines that run both. */
        for (const QString &root : roots) {
            for (const char *sub : {"/fanshell-qs/sounds", "/hypr/sounds"}) {
                QDir dir(root + QLatin1String(sub));
                const QStringList files = dir.entryList(
                    {QStringLiteral("audio-volume-change.*")}, QDir::Files,
                    QDir::Name);
                if (!files.isEmpty())
                    return dir.absoluteFilePath(files.first());
            }
        }
        roots = QStandardPaths::standardLocations(
            QStandardPaths::GenericDataLocation);
        for (const QString &root : roots) {
            QDir dir(root + QStringLiteral(
                                "/sounds/freedesktop/stereo"));
            const QStringList files = dir.entryList(
                {QStringLiteral("audio-volume-change.*")}, QDir::Files,
                QDir::Name);
            if (!files.isEmpty())
                return dir.absoluteFilePath(files.first());
        }
        return QString();
    }();
    return sound;
}

} // namespace

HardwareControls *HardwareControls::instance()
{
    static HardwareControls *controls = new HardwareControls;
    return controls;
}

HardwareControls::HardwareControls(QObject *parent) : QObject(parent)
{
    const QDir leds(QStringLiteral("/sys/class/leds"));
    const QStringList devices = leds.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &device : devices) {
        const QString lower = device.toLower();
        if ((lower.contains(QLatin1String("kbd_backlight"))
             || lower.contains(QLatin1String("keyboard")))
            && QFileInfo::exists(leds.absoluteFilePath(
                device + QStringLiteral("/max_brightness")))) {
            m_keyboardDevice = device;
            break;
        }
    }
}

void HardwareControls::volumeUp()
{
    adjustVolume(1);
}

void HardwareControls::volumeDown()
{
    adjustVolume(-1);
}

void HardwareControls::adjustVolume(int direction)
{
    PulseBackend *pulse = PulseBackend::instance();
    if (!pulse->hasSink)
        return;
    if (pulse->sinkMuted) {
        pulse->setSinkMuted(false);
        HardwareOsd::instance()->showVolume(qRound(pulse->sinkVolume * 100),
                                            false);
        return;
    }
    const int current = qRound(pulse->sinkVolume * 100);
    const int next = qBound(0, current + direction * audioStep, 100);
    pulse->setSinkVolume(next / 100.0);
    HardwareOsd::instance()->showVolume(next, false);
    playVolumeSound();
}

void HardwareControls::toggleVolumeMute()
{
    PulseBackend *pulse = PulseBackend::instance();
    if (!pulse->hasSink)
        return;
    const bool muted = !pulse->sinkMuted;
    pulse->setSinkMuted(muted);
    HardwareOsd::instance()->showVolume(qRound(pulse->sinkVolume * 100),
                                        muted);
}

void HardwareControls::toggleMicrophoneMute()
{
    PulseBackend *pulse = PulseBackend::instance();
    if (!pulse->hasSource)
        return;
    const bool muted = !pulse->sourceMuted;
    pulse->setSourceMuted(muted);
    HardwareOsd::instance()->showMicrophone(
        qRound(pulse->sourceVolume * 100), muted);
}

void HardwareControls::playVolumeSound()
{
    const QString sound = eventSound();
    if (sound.isEmpty())
        return;
    QString player = QStandardPaths::findExecutable(QStringLiteral("pw-play"));
    if (player.isEmpty())
        player = QStandardPaths::findExecutable(QStringLiteral("paplay"));
    if (!player.isEmpty())
        QProcess::startDetached(player, {sound});
}

int HardwareControls::steppedLevel(int current, int direction)
{
    current = qBound(1, current, 100);
    if (direction > 0)
        return current < 5 ? 5 : qMin(100, current + 5);
    return current <= 5 ? 1 : qMax(5, current - 5);
}

void HardwareControls::brightnessUp()
{
    adjustSysfs(false, 1);
}

void HardwareControls::brightnessDown()
{
    adjustSysfs(false, -1);
}

void HardwareControls::keyboardBacklightUp()
{
    if (hasKeyboardBacklight())
        adjustSysfs(true, 1);
}

void HardwareControls::keyboardBacklightDown()
{
    if (hasKeyboardBacklight())
        adjustSysfs(true, -1);
}

void HardwareControls::adjustSysfs(bool keyboard, int direction)
{
    int &steps = keyboard ? m_keyboardSteps : m_backlightSteps;
    bool &busy = keyboard ? m_keyboardBusy : m_backlightBusy;
    steps += direction;
    if (!busy)
        runSysfsQueue(keyboard);
}

void HardwareControls::runSysfsQueue(bool keyboard)
{
    int &steps = keyboard ? m_keyboardSteps : m_backlightSteps;
    bool &busy = keyboard ? m_keyboardBusy : m_backlightBusy;
    if (!steps) {
        busy = false;
        return;
    }
    busy = true;
    QStringList query;
    if (keyboard)
        query << QStringLiteral("--device") << m_keyboardDevice;
    else
        query << QStringLiteral("--class=backlight");
    query << QStringLiteral("-m");
    run(QStringLiteral("brightnessctl"), query,
        [this, keyboard](int status, const QByteArray &output) {
            int &pending = keyboard ? m_keyboardSteps : m_backlightSteps;
            bool &active = keyboard ? m_keyboardBusy : m_backlightBusy;
            const int current = percentFromBrightnessctl(output);
            if (status != 0 || current < 0) {
                pending = 0;
                active = false;
                notifyFailure(keyboard ? QStringLiteral("Keyboard backlight")
                                       : QStringLiteral("Brightness"),
                              QStringLiteral("No supported brightness device was found"));
                return;
            }
            const int queued = pending;
            pending = 0;
            int next = current;
            const int direction = queued > 0 ? 1 : -1;
            for (int i = 0; i < qAbs(queued); ++i)
                next = steppedLevel(next, direction);
            QStringList set;
            if (keyboard)
                set << QStringLiteral("--device") << m_keyboardDevice;
            else
                set << QStringLiteral("--class=backlight");
            set << QStringLiteral("set")
                << QString::number(next) + QLatin1Char('%');
            run(QStringLiteral("brightnessctl"), set,
                [this, keyboard, next](int setStatus, const QByteArray &) {
                    bool &running = keyboard ? m_keyboardBusy : m_backlightBusy;
                    running = false;
                    if (setStatus == 0) {
                        if (keyboard)
                            HardwareOsd::instance()->showKeyboardBacklight(next);
                        else
                            HardwareOsd::instance()->showBrightness(next, false);
                    } else {
                        notifyFailure(keyboard
                                          ? QStringLiteral("Keyboard backlight")
                                          : QStringLiteral("Brightness"),
                                      QStringLiteral("Could not change brightness"));
                    }
                    runSysfsQueue(keyboard);
                });
        });
}

void HardwareControls::monitorBrightnessUp()
{
    adjustDdc(1);
}

void HardwareControls::monitorBrightnessDown()
{
    adjustDdc(-1);
}

void HardwareControls::adjustDdc(int direction)
{
    m_ddcSteps += direction;
    if (!m_ddcBusy)
        runDdcQueue();
}

void HardwareControls::discoverDdc()
{
    m_ddcBusy = true;
    run(QStringLiteral("ddcutil"),
        {QStringLiteral("detect"), QStringLiteral("--brief")},
        [this](int status, const QByteArray &output) {
            m_ddcDiscovered = true;
            m_ddcBuses.clear();
            const QRegularExpression re(QStringLiteral("/dev/i2c-(\\d+)"));
            QRegularExpressionMatchIterator matches =
                re.globalMatch(QString::fromLocal8Bit(output));
            while (matches.hasNext())
                m_ddcBuses << matches.next().captured(1);
            m_ddcBuses.removeDuplicates();
            m_ddcBusy = false;
            if (status != 0 || m_ddcBuses.isEmpty()) {
                m_ddcSteps = 0;
                notifyFailure(QStringLiteral("Monitor brightness"),
                              QStringLiteral("No DDC-capable display was found"));
                return;
            }
            runDdcQueue();
        });
}

void HardwareControls::runDdcQueue()
{
    if (!m_ddcSteps) {
        m_ddcBusy = false;
        return;
    }
    if (!m_ddcDiscovered) {
        discoverDdc();
        return;
    }
    if (m_ddcBuses.isEmpty()) {
        m_ddcSteps = 0;
        m_ddcBusy = false;
        return;
    }
    m_ddcBusy = true;
    run(QStringLiteral("ddcutil"),
        {QStringLiteral("getvcp"), QStringLiteral("0x10"),
         QStringLiteral("--brief"), QStringLiteral("--bus"),
         m_ddcBuses.first()},
        [this](int status, const QByteArray &output) {
            const QRegularExpression re(
                QStringLiteral("VCP\\s+10\\s+C\\s+(\\d+)\\s+(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch match =
                re.match(QString::fromLocal8Bit(output));
            if (status != 0 || !match.hasMatch()
                || match.captured(2).toInt() <= 0) {
                m_ddcSteps = 0;
                m_ddcBusy = false;
                m_ddcDiscovered = false;
                notifyFailure(QStringLiteral("Monitor brightness"),
                              QStringLiteral("Could not read display brightness"));
                return;
            }
            const int maximum = match.captured(2).toInt();
            int next = qRound(match.captured(1).toInt() * 100.0 / maximum);
            const int queued = m_ddcSteps;
            m_ddcSteps = 0;
            const int direction = queued > 0 ? 1 : -1;
            for (int i = 0; i < qAbs(queued); ++i)
                next = steppedLevel(next, direction);
            const int raw = qRound(next * maximum / 100.0);
            auto remaining = std::make_shared<int>(m_ddcBuses.size());
            auto failed = std::make_shared<bool>(false);
            for (const QString &bus : m_ddcBuses) {
                run(QStringLiteral("ddcutil"),
                    {QStringLiteral("setvcp"), QStringLiteral("0x10"),
                     QString::number(raw), QStringLiteral("--bus"), bus},
                    [this, remaining, failed, next](int setStatus,
                                                    const QByteArray &) {
                        if (setStatus != 0)
                            *failed = true;
                        if (--*remaining)
                            return;
                        m_ddcBusy = false;
                        if (*failed) {
                            m_ddcDiscovered = false;
                            notifyFailure(
                                QStringLiteral("Monitor brightness"),
                                QStringLiteral("Could not change every display"));
                        } else {
                            HardwareOsd::instance()->showBrightness(next, true);
                        }
                        runDdcQueue();
                    });
            }
        });
}

void HardwareControls::notifyFailure(const QString &title,
                                     const QString &detail)
{
    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"), 2);
    NotificationService::instance()->Notify(
        QStringLiteral("brightness"), 0, QString(), title, detail,
        QStringList(), hints, 0);
}

void HardwareControls::run(
    const QString &program, const QStringList &arguments,
    std::function<void(int, const QByteArray &)> done)
{
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto completed = std::make_shared<bool>(false);
    auto callback = std::make_shared<
        std::function<void(int, const QByteArray &)>>(std::move(done));
    auto finish = [process, completed, callback](int status) {
        if (*completed)
            return;
        *completed = true;
        const QByteArray output = process->readAll();
        process->deleteLater();
        (*callback)(status, output);
    };
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            process, [finish](int status, QProcess::ExitStatus exitStatus) {
                finish(exitStatus == QProcess::NormalExit ? status : -1);
            });
    connect(process, &QProcess::errorOccurred, process,
            [finish](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finish(-1);
            });
    /* kill() makes finished() fire with CrashExit, which reports -1. */
    QTimer::singleShot(helperTimeoutMs, process, [process]() {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
    process->start(program, arguments);
}
