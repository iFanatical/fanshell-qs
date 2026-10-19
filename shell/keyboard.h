/* Keyboard settings, from the Control Panel's Keyboard tile:
 *
 *     fanshell-qs-shell ipc call keyboard toggle|show|hide
 *
 *  - Num Lock on at login (the default): numlockx's one job, done here with
 *    the same XKB call. It is applied once per X session, when the shell
 *    first starts in it -- a marker on the root window keeps a later restart
 *    of the shell from switching Num Lock back on after it was turned off
 *    by hand.
 *  - Key repeat delay and rate, set through XKB as `xset r rate` would.
 *  - What Caps Lock does, and a Compose key: XKB options, applied to the
 *    keymap in-process the way setxkbmap -option does (libxkbfile reads
 *    the rules), so setxkbmap need not be installed. Options the shell does
 *    not manage, from xorg.conf say, are kept.
 *  - An on-screen display when Caps Lock or Num Lock changes.
 *  - On a machine with a keyboard backlight, switching it off after a time
 *    without input, and back on at the next key or mouse movement.
 *
 * A keyboard plugged in later starts with the server's defaults, so the
 * repeat rate and the options are applied again when one is added. Saved
 * in ~/.config/fanshell-qs/keyboard.conf. */
#ifndef FANSHELL_QS_KEYBOARD_H
#define FANSHELL_QS_KEYBOARD_H

#include <QObject>
#include <QString>
#include <QTimer>

class KeyboardWindow;

class KeyboardSettings : public QObject {
public:
    struct Settings {
        bool numlock = true;
        int repeatDelay = 200;      /* ms before a held key repeats */
        int repeatRate = 35;        /* repeats per second */
        QString caps = QStringLiteral("default"); /* escape, ctrl, swap, none */
        QString compose = QStringLiteral("none"); /* ralt, menu, rctrl */
        bool lockOsd = true;
        int backlightIdle = 0;      /* seconds without input; 0 = never */
    };

    static KeyboardSettings *instance();

    Settings settings() const { return m_settings; }
    /* Apply and save. Turning Num Lock at login on also turns it on now. */
    void set(const Settings &s);

    void openWindow();
    void toggleWindow();
    void hideWindow();

    /* From the X event filter. */
    void keyboardAdded();
    void lockStateChanged(unsigned int lockedMods);

private:
    explicit KeyboardSettings(QObject *parent = nullptr);
    void applyAtLogin();
    void applyRepeat();
    void applyOptions(bool force);
    void quiet();
    void backlightTick();
    void backlightRestore();

    Settings m_settings;
    KeyboardWindow *m_win = nullptr;

    unsigned int m_locked = 0;      /* the last locked modifiers seen */
    qint64 m_quietUntil = 0;        /* no lock OSD for the shell's own changes */
    QTimer m_hotplug;               /* collapses a burst of hierarchy events */

    QTimer m_backlightTimer;
    int m_backlightRestore = -1;    /* the level to restore; -1 = not off */
    qint64 m_lastIdle = 0;
};

#endif
