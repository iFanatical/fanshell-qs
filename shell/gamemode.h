/* Game mode: one switch for what gets in a game's way.
 *
 *     fanshell-qs-shell ipc call gamemode toggle|on|off
 *
 * While on: notifications are held back (do not disturb), the night light
 * leaves the gamma ramps alone (each write stalls the X server for a few
 * frames), the power profile is "performance" where there is one, and the
 * screen neither blanks nor locks (a controller is not X input). Turning it
 * off puts back the do-not-disturb setting and power profile it found, and
 * lets the night light catch up. Under Hyprland it also switches off the
 * compositor's animations, blur and shadows, and back on after.
 *
 * gamemode.auto=fullscreen in shell.conf turns it on by itself while the
 * focused window is fullscreen, and off again after. Switched off by hand
 * meanwhile, it stays off until that window stops being fullscreen. */
#ifndef FANSHELL_QS_GAMEMODE_H
#define FANSHELL_QS_GAMEMODE_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class GameMode : public QObject {
    Q_OBJECT
public:
    static GameMode *instance();

    bool active() const { return m_active; }
    void setActive(bool on);
    void toggle() { setActive(!m_active); }

signals:
    void changed();

private:
    explicit GameMode(QObject *parent = nullptr);
    void enable(bool on);
    void watchFullscreen();
    /* Hyprland only: animations, blur and shadows on or (saved) off. */
    void setCompositorEffects(bool on);

    bool m_active = false;
    /* What turning it off puts back. */
    bool m_dndBefore = false;
    QString m_profileBefore;
    /* gamemode.auto=fullscreen. */
    QTimer m_watch;
    bool m_autoOn = false;
    bool m_suppressed = false;
    /* Hyprland options game mode switched off, and whether they were on. */
    QHash<QString, bool> m_effectsBefore;
};

#endif
