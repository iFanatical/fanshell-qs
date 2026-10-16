/* Native IPC-facing hardware controls. Audio uses the existing libpulse
 * backend; brightness helpers are asynchronous so DDC/I2C can never block
 * the shell's UI thread. */
#ifndef FANSHELL_QS_HARDWARECONTROLS_H
#define FANSHELL_QS_HARDWARECONTROLS_H

#include <QObject>
#include <QStringList>

#include <functional>

class HardwareControls : public QObject {
    Q_OBJECT
public:
    static HardwareControls *instance();

    void volumeUp();
    void volumeDown();
    void toggleVolumeMute();
    void toggleMicrophoneMute();

    void brightnessUp();
    void brightnessDown();
    void monitorBrightnessUp();
    void monitorBrightnessDown();
    void keyboardBacklightUp();
    void keyboardBacklightDown();
    bool hasKeyboardBacklight() const { return !m_keyboardDevice.isEmpty(); }

private:
    explicit HardwareControls(QObject *parent = nullptr);
    void adjustVolume(int direction);
    void playVolumeSound();
    void adjustSysfs(bool keyboard, int direction);
    void runSysfsQueue(bool keyboard);
    void adjustDdc(int direction);
    void runDdcQueue();
    void discoverDdc();
    void notifyFailure(const QString &title, const QString &detail);
    void run(const QString &program, const QStringList &arguments,
             std::function<void(int, const QByteArray &)> done);
    static int steppedLevel(int current, int direction);

    QString m_keyboardDevice;
    QStringList m_ddcBuses;
    int m_backlightSteps = 0;
    int m_keyboardSteps = 0;
    int m_ddcSteps = 0;
    bool m_backlightBusy = false;
    bool m_keyboardBusy = false;
    bool m_ddcBusy = false;
    bool m_ddcDiscovered = false;
};

#endif
