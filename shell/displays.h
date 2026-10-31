/* Display settings: output layout (on/off, resolution, refresh, rotation,
 * position, primary, TearFree where the driver has it; under Hyprland scale
 * and adaptive sync instead of primary and TearFree) and digital vibrance,
 * in a centred popup.
 *
 *     fanshell-qs-shell ipc call outputs toggle|show|hide
 *
 * State is read straight from RandR over xcb, or from Hyprland's `monitors
 * all`. Changes are made through scripts/fanshell-qs-outputs, the one place
 * that turns a layout into an xrandr command (or Hyprland monitor rules): the popup writes the edited layout to a file in
 * $XDG_RUNTIME_DIR and has the script apply it. A change is a trial until
 * kept -- it reverts by itself after a countdown, so a mode the monitor
 * cannot show cannot leave you stuck. Keeping it has the script merge it into
 * ~/.config/fanshell-qs/outputs.conf, under the key for the set of outputs
 * connected now; .xinitrc applies that at login, and the shell itself under
 * Hyprland, where output names differ (DP-1, not DisplayPort-0) and so do
 * the saved sections.
 *
 * DisplaySettings also watches RandR, or Hyprland's monitor events: when
 * the set of connected outputs changes (a monitor plugged in or removed),
 * it applies the layout saved for the new set, if there is one. Hyprland
 * also puts its own monitor rules back on a config reload, so the saved
 * layout is applied again then. */
#ifndef FANSHELL_QS_DISPLAYS_H
#define FANSHELL_QS_DISPLAYS_H

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>

namespace Displays {

struct Mode {
    int w = 0, h = 0;
    double rate = 0;
};

struct Output {
    QString name;
    bool enabled = false;
    int x = 0, y = 0;
    int w = 0, h = 0; /* the mode, before rotation */
    double rate = 0;
    QString rotate = QStringLiteral("normal"); /* normal|left|right|inverted */
    bool primary = false;
    /* The driver's TearFree output property: off|on|auto, or empty when the
     * driver has none (amdgpu and recent modesetting do). */
    QString tearFree;
    /* Largest resolution first, then fastest refresh. */
    QVector<Mode> modes;
    Mode preferred;
    /* Hyprland: the output's scale, and adaptive sync -- off|on|fullscreen,
     * or empty where it cannot be set (X11). */
    double scale = 1.0;
    QString vrr;

    bool sideways() const
    {
        return rotate == QLatin1String("left")
               || rotate == QLatin1String("right");
    }
    /* In layout coordinates, which a scale divides. */
    int width() const { return qRound((sideways() ? h : w) / scale); }
    int height() const { return qRound((sideways() ? w : h) / scale); }
    QRect rect() const { return QRect(x, y, width(), height()); }
};

/* Connected outputs, sorted by name. */
QVector<Output> query();
/* The layout key fanshell-qs-outputs uses: names joined with '+'. */
QString key(const QVector<Output> &outputs);
/* One [key] section in fanshell-qs-outputs' format. */
QString profile(const QVector<Output> &outputs);

} /* namespace Displays */

class DisplaysWindow;

class DisplaySettings : public QObject {
    Q_OBJECT
public:
    static DisplaySettings *instance();

    void openWindow();
    void toggleWindow();
    void hideWindow();

    /* Runs fanshell-qs-outputs ARGS; `done` gets its exit code and stderr. */
    void runScript(const QStringList &args,
                   std::function<void(int, const QString &)> done = {});
    /* Applies `outputs` as a trial (no save) / merges it into outputs.conf. */
    void applyLayout(const QVector<Displays::Output> &outputs,
                     std::function<void(int, const QString &)> done);
    void saveLayout(const QVector<Displays::Output> &outputs);

    /* Called for every RandR event Qt receives, and Hyprland monitor
     * event. */
    void randrEvent();
    /* The window manager put its own output config back (Hyprland's
     * reload): apply the saved layout again even for the same outputs. */
    void outputsReset();

private:
    explicit DisplaySettings(QObject *parent = nullptr);
    void settle();
    QString writeTemp(const QString &name, const QString &text);

    DisplaysWindow *m_win = nullptr;
    QTimer m_settle;
    QString m_lastKey;
    bool m_forceApply = false;
};

/* Screen blanking and DPMS, set over xcb (core screen saver + the DPMS
 * extension), in a popup of their own (Control Panel > Screen power):
 *
 *     fanshell-qs-shell ipc call dpms toggle|show|hide|off
 *
 * Settings are saved to dpms.conf in Paths::configDir() and applied at
 * startup. Without a dpms.conf the default is what .xinitrc used to ask of
 * xset: no blanking and DPMS off. */
class ScreenPowerWindow;

class ScreenPower : public QObject {
    Q_OBJECT
public:
    struct Settings {
        int blank = 0;    /* seconds of idle before blanking; 0 = never */
        bool dpms = false;
        int standby = 0;  /* seconds; 0 = that stage never */
        int suspend = 0;
        int off = 0;
    };

    static ScreenPower *instance();

    /* What the X server is doing now. */
    Settings live() const;
    /* Apply and save. Timeouts are made non-decreasing first, as X wants. */
    void set(Settings s);
    /* Power the displays down until the next input. */
    void turnOffNow();

    void openWindow();
    void toggleWindow();
    void hideWindow();

private:
    explicit ScreenPower(QObject *parent = nullptr);
    void apply(const Settings &s);
    void save(const Settings &s);

    ScreenPowerWindow *m_win = nullptr;
    /* turnOffNow() on a server with DPMS disabled enables it for the
     * moment; this watches for the wake-up to disable it again. */
    QTimer m_wake;
    bool m_enabledForOff = false;
};

/* Locking the screen on its own: after a stretch of idle, and before the
 * machine sleeps. The lock itself is scripts/fanshell-qs-lock, which runs
 * this binary's lock screen (lockscreen.h). Configured from Control Panel >
 * Screen lock:
 *
 *     fanshell-qs-shell ipc call lock toggle|show|hide|now
 *
 * Idle time is the X screen saver extension's time since the last input, so
 * it is independent of blanking. Lock before suspend holds a logind "delay"
 * inhibitor and releases it once the screen is locked. Settings are saved to
 * lock.conf in Paths::configDir(); without one, the screen locks after 10
 * minutes idle, before suspend, and not while a window is fullscreen, and
 * the lock screen powers the displays down after 5 minutes.
 *
 * The lock screen tells the shell when it locks and unlocks (`lock locked`,
 * `lock unlocked` on the control socket), and the shell watches the lock
 * file too, so it knows even after restarting; while locked it shows no
 * toasts or OSDs. */
class ScreenLockWindow;
class QProcess;

class ScreenLock : public QObject {
    Q_OBJECT
public:
    struct Settings {
        int after = 600;            /* seconds idle before locking; 0 = never */
        bool beforeSuspend = true;
        bool skipFullscreen = true; /* not while the focused window is
                                       fullscreen (video, games) */
        bool skipAudio = true;      /* not while a program plays audio
                                       (music, a call, a video) */
        int displayOff = 300;       /* seconds on the lock screen without
                                       input before the displays power
                                       down; 0 = never */
    };

    static ScreenLock *instance();

    Settings settings() const { return m_settings; }
    /* Apply and save. */
    void set(const Settings &s);
    /* Lock now; `done` runs once the screen is locked (or locking failed). */
    void lockNow(std::function<void()> done = {});

    void openWindow();
    void toggleWindow();
    void hideWindow();

    /* What is holding the idle lock off now -- programs that asked
     * (idleinhibit.h), audio playing -- or empty. */
    QStringList pausedBy() const;

    bool locked() const { return m_locked; }
    /* The lock screen's word, or what the lock file says. */
    void setLocked(bool locked);

signals:
    /* About to lock: anything holding a keyboard grab should let go. */
    void aboutToLock();
    /* fanshell-qs-lock has returned: the screen is locked, or it failed. */
    void lockerDone();
    void lockedChanged(bool locked);

private slots:
    void prepareForSleep(bool sleeping);

private:
    explicit ScreenLock(QObject *parent = nullptr);
    void tick();
    void takeSleepInhibitor();
    void releaseSleepInhibitor();

    Settings m_settings;
    ScreenLockWindow *m_win = nullptr;
    QTimer m_idle;
    /* Locked for this stretch of idle; cleared by the next input. */
    bool m_fired = false;
    qint64 m_lastIdle = 0;
    QElapsedTimer m_age;
    QProcess *m_locker = nullptr;
    int m_sleepFd = -1;
    bool m_locked = false;
    /* While locked: the lock file, for a lock screen gone without a word. */
    QTimer m_lockWatch;
};

#endif
