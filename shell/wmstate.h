/* The window manager as the shell sees it, whichever one is running.
 *
 * The panels, pills, launcher, toasts and popups only ever talk to WmState:
 * the outputs, each one's workspaces, layout and focused title, and a few
 * actions. A backend subclass (OxwmState, QtileState) fills the model from
 * its window manager and carries out the actions; everything here is shared.
 * instance() picks the backend for the window manager that is running.
 *
 * Workspaces are an indexed list with per-monitor bitmasks, which fits both
 * models in use:
 *   - dwm-like (oxwm): every monitor has its own view of the same tag set,
 *     so each monitor's masks differ, and several tags can be viewed at once.
 *   - global (qtile groups, Hyprland workspaces): each workspace is shown on
 *     at most one monitor. `selected` is what this monitor shows, and
 *     `elsewhere` is what the other monitors show.
 *
 * Monitors are matched to the QScreen covering the same rectangle and take
 * that screen's name, so the rest of the shell finds "its" monitor by
 * QScreen::name() whatever the window manager calls it. */
#ifndef FANSHELL_QS_WMSTATE_H
#define FANSHELL_QS_WMSTATE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

class WmState : public QObject {
    Q_OBJECT
public:
    struct Monitor {
        int id = 0;          /* the window manager's own monitor number */
        QString name;        /* QScreen::name() of the matching screen */
        int x = 0, y = 0, w = 0, h = 0;  /* device pixels */
        bool focused = false;
        /* Bit i is workspaceNames[i]. */
        quint32 selected = 0;   /* shown on this monitor */
        quint32 elsewhere = 0;  /* shown on another monitor (global models) */
        quint32 occupied = 0;   /* has windows */
        quint32 urgent = 0;
        QString layout;      /* "tiling", "monocle", ... */
        QString title;       /* focused window on this monitor */
        /* A window on what this monitor shows is fullscreen: the panel
         * hides, as i3's bar does. */
        bool fullscreen = false;
        /* Labels for this monitor's own workspaces, when they differ per
         * monitor (Hyprland's blocks: 1-5 on one output, 6-10 on the next);
         * empty to use workspaceNames. */
        QStringList labels;
    };

    /* What the backend can do, so the UI offers only that. */
    struct Features {
        /* View several workspaces at once (dwm tags); right-click toggles. */
        bool toggleWorkspaces = false;
        bool cycleLayout = false;
        /* Keybind submaps (Hyprland): the bar shows the active one. */
        bool submaps = false;
    };

    /* The shared instance, for the window manager that is running. */
    static WmState *instance();

    QVector<Monitor> monitors;
    QStringList workspaceNames;
    bool connected = false;
    /* The active keybind submap; empty in the default one. */
    QString submap;

    virtual QString wmName() const = 0;
    virtual Features features() const = 0;
    /* Why the bar has no workspaces or title yet, for the log; empty when
     * the window manager is reachable. */
    virtual QString unavailableHint() const = 0;

    const Monitor *monitorByName(const QString &name) const;
    /* The output that hosts the tray and notifications: X's primary output
     * when the window manager has a monitor there, else its lowest-numbered
     * one. Stable as focus moves, unlike focusedMonitor(). */
    QString primaryMonitor() const;
    /* The output the window manager has focused. This is what the launcher
     * opens on, so it lands where the user is working. Falls back to the
     * primary before the first refresh. */
    QString focusedMonitor() const;

    /* Workspaces are zero-based indexes into workspaceNames. */
    virtual void viewWorkspace(int monitorId, int index) = 0;
    virtual void toggleWorkspace(int monitorId, int index);
    virtual void cycleLayout(int monitorId);
    /* Leave the active submap. */
    virtual void resetSubmap();
    /* Whether the focused monitor shows a fullscreen window. */
    bool focusedFullscreen() const;

    /* The window manager reports a change through the shell's own control
     * socket (`fanshell-qs-shell ipc call wm changed`), for one with no
     * event stream of its own (qtile). Coalesced like any other change. */
    void wmChanged() { scheduleRefresh(); }

    /* X window ids of the managed windows, and focusing one of them. */
    virtual QVector<quintptr> windows() = 0;
    virtual bool focusWindow(quintptr window) = 0;
    /* Focus the window of process `pid` (a notification's sender), when it
     * has exactly one. By default found through windows()' _NET_WM_PID. */
    virtual bool focusPid(uint pid);

signals:
    void changed();
    /* Cheap title-only update for one panel; does not imply any workspace,
     * monitor, tray-host, or layout state changed. */
    void titleChanged(const QString &monitorName);

protected:
    explicit WmState(QObject *parent = nullptr);

    /* Backends call these. scheduleRefresh() coalesces a burst of change
     * notifications into one refresh() on the next event-loop turn. */
    void scheduleRefresh();
    virtual void refresh() = 0;
    /* Replace the model and emit changed() if anything differs. Names the
     * monitors after their screens first. A transient empty monitor list
     * (the window manager mid-restart) keeps the last good one. */
    void publish(QVector<Monitor> mons, const QStringList &names);
    void setDisconnected();
    /* Update one monitor's title in place; false if the monitor is unknown,
     * in which case the caller should refresh instead. */
    bool updateTitle(int monitorId, const QString &title);

private:
    void nameMonitors(QVector<Monitor> &mons) const;

    QTimer m_coalesce;
};

#endif
