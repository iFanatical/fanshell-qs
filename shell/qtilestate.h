/* The qtile backend for WmState.
 *
 * State comes from one eval on qtile's IPC socket (qtileipc.h), which
 * answers a JSON snapshot of the screens, the focused screen and every
 * group, so a refresh is a single consistent round trip. qtile has no event
 * stream: hooks in its config.py send `wm changed` over the shell's own
 * control socket (ipc.h), and the shell re-queries (fanqtile-qs's
 * fanqtile/shell.py does this). Without those hooks the bar shows the state
 * as it was at startup.
 *
 * qtile's groups are global, each shown on at most one screen. Two ways of
 * putting them on the bar:
 *   - per monitor, when groups carry a screen_affinity (fanqtile-qs keeps it
 *     on the qtile screen each group belongs to): every monitor lists only
 *     its own groups, by label, like oxwm's per-monitor tags. Groups with no
 *     affinity are left out.
 *   - global otherwise: every monitor lists every group; `selected` is the
 *     one this screen shows and `elsewhere` the ones other screens show.
 * Groups named "special:*" (fanqtile-qs's special workspaces) and
 * ScratchPads are never on the bar.
 *
 * Actions use qtile's ordinary commands: a group's toscreen, the root's
 * to_screen and next_layout. */
#ifndef FANSHELL_QS_QTILESTATE_H
#define FANSHELL_QS_QTILESTATE_H

#include <QHash>
#include <QTimer>

#include "wmstate.h"

class QtileState : public WmState {
    Q_OBJECT
public:
    explicit QtileState(QObject *parent = nullptr);

    QString wmName() const override { return QStringLiteral("qtile"); }
    Features features() const override;
    QString unavailableHint() const override;

    void viewWorkspace(int monitorId, int index) override;
    void cycleLayout(int monitorId) override;

    QVector<quintptr> windows() override;
    bool focusWindow(quintptr window) override;

protected:
    void refresh() override;

private:
    /* Group names behind each monitor's workspace indexes, from the last
     * refresh: monitor id (qtile's screen index) -> names by index. */
    QHash<int, QStringList> m_groups;
    /* The group each monitor shows, for cycleLayout. */
    QHash<int, QString> m_shown;
    /* Retries while qtile is unreachable (restarting, or not up yet). */
    QTimer m_retry;
};

#endif
