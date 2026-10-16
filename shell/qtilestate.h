/* The qtile backend for WmState.
 *
 * Not written yet: this placeholder reports no monitors and ignores every
 * action, so the shell builds and runs with an empty workspace row. The
 * plan for the real one:
 *
 *   - state: qtile's groups as the workspace list. They are global, so each
 *     monitor's `selected` is the group its screen shows and `elsewhere` the
 *     groups the other screens show; `occupied` and `urgent` are the same on
 *     every monitor. Screens, layouts and the focused window's title come
 *     from the same query.
 *   - changes: qtile has no event stream, so hooks in config.py (setgroup,
 *     focus_change, client_name_updated, layout_change, screen_change, ...)
 *     send a line to the shell, which re-queries (OxwmState
 *     works the same way over oxwm's event socket).
 *   - actions: `qtile cmd-obj` (or its IPC socket directly) to show a group
 *     on a screen, cycle the layout and focus a window. */
#ifndef FANSHELL_QS_QTILESTATE_H
#define FANSHELL_QS_QTILESTATE_H

#include "wmstate.h"

class QtileState : public WmState {
    Q_OBJECT
public:
    explicit QtileState(QObject *parent = nullptr) : WmState(parent) {}

    QString wmName() const override { return QStringLiteral("qtile"); }
    Features features() const override { return {}; }
    QString unavailableHint() const override
    {
        return QStringLiteral("the qtile backend is not written yet.");
    }

    void viewWorkspace(int, int) override {}
    QVector<quintptr> windows() override { return {}; }
    bool focusWindow(quintptr) override { return false; }

protected:
    void refresh() override {}
};

#endif
