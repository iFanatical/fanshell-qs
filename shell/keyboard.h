/* Keyboard settings, from the Control Panel's Keyboard tile:
 *
 *     fanshell-qs-shell ipc call keyboard toggle|show|hide
 *
 * Num Lock on at login (the default): numlockx's one job, done here with
 * the same XKB call, so no separate program is needed. It is applied once
 * per X session, when the shell first starts in it -- a marker on the root
 * window keeps a later restart of the shell from switching Num Lock back on
 * after it was turned off by hand. Saved in ~/.config/fanshell-qs/
 * keyboard.conf. */
#ifndef FANSHELL_QS_KEYBOARD_H
#define FANSHELL_QS_KEYBOARD_H

#include <QObject>

class KeyboardWindow;

class KeyboardSettings : public QObject {
public:
    struct Settings {
        bool numlock = true;
    };

    static KeyboardSettings *instance();

    Settings settings() const { return m_settings; }
    /* Save; turning Num Lock at login on also turns it on now. */
    void set(const Settings &s);

    void openWindow();
    void toggleWindow();
    void hideWindow();

private:
    explicit KeyboardSettings(QObject *parent = nullptr);
    void applyAtLogin();

    Settings m_settings;
    KeyboardWindow *m_win = nullptr;
};

#endif
