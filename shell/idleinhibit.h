/* org.freedesktop.ScreenSaver on the session bus: how programs ask the
 * desktop not to blank or lock while they are in use -- a browser playing
 * a video, Discord in a call, a video player. Nothing else in these
 * sessions provides it, so without this their requests go unanswered.
 *
 * Inhibit(application, reason) returns a cookie that UnInhibit(cookie)
 * gives back; a program that exits without doing so has its requests
 * dropped when its bus name goes away. While any are held:
 *
 *  - the screen lock (ScreenLock, displays.h) does not lock on idle;
 *  - the X screen saver and DPMS are suspended (XScreenSaverSuspend), so
 *    Screen power does not blank the screen either.
 *
 * Keep awake (System dropdown, `ipc call keepawake toggle`) and game mode
 * are requests of the shell's own, held until turned off and not saved.
 *
 * Served at /org/freedesktop/ScreenSaver and /ScreenSaver, where different
 * programs look. If another process already owns the name, it is left to
 * it. */
#ifndef FANSHELL_QS_IDLEINHIBIT_H
#define FANSHELL_QS_IDLEINHIBIT_H

#include <QDBusContext>
#include <QHash>
#include <QObject>
#include <QStringList>

class QDBusServiceWatcher;

class IdleInhibitor : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    static IdleInhibitor *instance();

    bool inhibited() const { return !m_inhibitors.isEmpty(); }
    /* "Brave: Playing video", one per request held. */
    QStringList reasons() const;

    /* The shell's own requests; programs' cookies start at 1. */
    static constexpr uint keepAwakeCookie = 0;
    static constexpr uint gameModeCookie = 0xffffffffu;
    bool holds(uint cookie) const { return m_inhibitors.contains(cookie); }
    void setShellRequest(uint cookie, const QString &name, bool on);
    bool keepAwake() const { return holds(keepAwakeCookie); }
    void setKeepAwake(bool on)
    {
        setShellRequest(keepAwakeCookie, QStringLiteral("Keep awake"), on);
    }

public slots:
    Q_SCRIPTABLE uint Inhibit(const QString &application, const QString &reason);
    Q_SCRIPTABLE void UnInhibit(uint cookie);
    /* The rest of the interface, answered plainly: the shell's lock is not
     * a screen saver these programs can drive. */
    Q_SCRIPTABLE bool GetActive() { return false; }
    Q_SCRIPTABLE uint GetActiveTime() { return 0; }
    Q_SCRIPTABLE uint GetSessionIdleTime();
    Q_SCRIPTABLE void SimulateUserActivity();

signals:
    void changed();

private:
    explicit IdleInhibitor(QObject *parent = nullptr);
    void update();

    struct Inhibitor {
        QString owner; /* the caller's unique bus name */
        QString application;
        QString reason;
    };
    QHash<uint, Inhibitor> m_inhibitors;
    uint m_next = 1;
    bool m_suspended = false;
    QDBusServiceWatcher *m_watcher;
};

#endif
