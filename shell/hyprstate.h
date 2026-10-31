/* The Hyprland backend of WmState (ported from fanhypr-qs's HyprState).
 *
 * Everything comes off HyprIpc: the .socket2.sock event stream says *that*
 * something changed, and a coalesced re-query of .socket.sock says *what*
 * it now is. A single user action fans out into several events (moving a
 * window emits movewindow + activewindow + workspace), so the re-query is
 * coalesced like every backend's.
 *
 * Workspaces are a fixed block per output, shown whether or not they exist
 * yet, so the bar never reflows as workspaces come and go. The blocks come
 * from Hyprland's own workspace rules (`workspace = 3, monitor:DP-1`, or
 * hl.workspace_rule in Lua) when there are any, and are otherwise the next
 * contiguous run of five per output (1-5, 6-10). Each monitor's labels are
 * its own block's ids (Monitor::labels).
 *
 * The title fast path: Hyprland reports every title change of every window
 * (windowtitlev2), many times a second for an animated one. A title change
 * of a window a panel shows updates that panel alone; anything else waits
 * for a debounce. */
#ifndef FANSHELL_QS_HYPRSTATE_H
#define FANSHELL_QS_HYPRSTATE_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include "hypripc.h"
#include "wmstate.h"

class HyprState : public WmState {
    Q_OBJECT
public:
    explicit HyprState(QObject *parent = nullptr);

    QString wmName() const override { return QStringLiteral("hyprland"); }
    Features features() const override;
    QString unavailableHint() const override;

    void viewWorkspace(int monitorId, int index) override;
    void resetSubmap() override;

    /* Hyprland client addresses, not X ids. */
    QVector<quintptr> windows() override;
    bool focusWindow(quintptr window) override;
    bool focusPid(uint pid) override;

protected:
    void refresh() override;

private:
    void queueTitleUpdate(const QString &data);
    void applyTitleUpdates();
    /* Re-read what only changes with the config: the workspace rules and
     * the submap. */
    void refreshConfig();

    HyprEventStream m_events;
    QTimer m_titleDebounce;
    QHash<QString, QString> m_pendingTitles; /* client address -> title */
    QHash<QString, int> m_shownClientMonitor; /* address -> monitor id */
    QSet<QString> m_knownClients;
    QString m_activeClient;
    /* Output name -> workspace ids pinned to it by the rules. */
    QHash<QString, QVector<int>> m_rules;
    /* Monitor id -> its block of workspace ids, from the last refresh. */
    QHash<int, QVector<int>> m_blocks;
};

#endif
