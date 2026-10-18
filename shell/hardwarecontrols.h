/* Native IPC-facing hardware controls. Audio uses the existing libpulse
 * backend; brightness helpers are asynchronous so DDC/I2C can never block
 * the shell's UI thread.
 *
 * The keyboard backlight moves in the device's own levels rather than in
 * percent: a ThinkPad's tpacpi::kbd_backlight has three (off, low, high),
 * and a 5% step from any of them rounds straight back to where it was. Its
 * level is read from sysfs (world-readable) and set through brightnessctl
 * (logind), and a change the firmware makes itself -- Fn+Space on a
 * ThinkPad -- is picked up from brightness_hw_changed. */
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
    /* Off, then each level up to the brightest, then off again: for a
     * keyboard with one backlight key. */
    void keyboardBacklightCycle();
    /* level is 0..keyboardMax(); osd shows the change on screen (for keys,
     * not for the System dropdown's slider, which shows it already). */
    void setKeyboardBacklight(int level, bool osd = false);
    bool hasKeyboardBacklight() const { return !m_keyboardDevice.isEmpty(); }
    int keyboardLevel() const { return m_keyboardLevel; }
    int keyboardMax() const { return m_keyboardMax; }
    /* Re-reads the level from sysfs, for a change made outside the shell. */
    void refreshKeyboardBacklight();

signals:
    void keyboardBacklightChanged();

private:
    explicit HardwareControls(QObject *parent = nullptr);
    void adjustVolume(int direction);
    void playVolumeSound();
    void adjustSysfs(int direction);
    void runSysfsQueue();
    void stepKeyboard(int direction);
    void runKeyboardQueue();
    void watchKeyboardFirmware();
    int readKeyboardLevel() const;
    void adjustDdc(int direction);
    void runDdcQueue();
    void discoverDdc();
    void notifyFailure(const QString &title, const QString &detail);
    void run(const QString &program, const QStringList &arguments,
             std::function<void(int, const QByteArray &)> done);
    static int steppedLevel(int current, int direction);

    QString m_keyboardDevice;
    int m_keyboardLevel = 0;
    int m_keyboardMax = 0;
    /* The level the last request asked for, while brightnessctl sets an
     * earlier one; -1 when nothing is waiting. */
    int m_keyboardPending = -1;
    bool m_keyboardPendingOsd = false;
    int m_keyboardHwFd = -1;
    QStringList m_ddcBuses;
    int m_backlightSteps = 0;
    int m_ddcSteps = 0;
    bool m_backlightBusy = false;
    bool m_keyboardBusy = false;
    bool m_ddcBusy = false;
    bool m_ddcDiscovered = false;
};

#endif
