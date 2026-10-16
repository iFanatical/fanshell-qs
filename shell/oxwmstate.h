/* The oxwm backend for WmState -- the oxwm counterpart of fanhypr-qs's
 * HyprState.
 *
 * Everything comes off OxwmIpc: the .socket2.sock event stream tells us
 * *that* something changed, and a coalesced `j/state` re-query tells us
 * *what* it now is. Coalescing matters because one user action fans out into
 * several events (viewing a tag emits tags + title + activewindow). Title
 * events are the exception: they carry the whole answer, so they update the
 * one monitor in place without a round trip.
 *
 * oxwm is dwm-shaped: every monitor has its own view of the same tag set,
 * with its own selected, occupied and urgent bitmasks and its own layout, and
 * several tags can be viewed at once. oxwm's monitors are Xinerama's, which
 * have no names, only geometry; WmState names them after their screens. */
#ifndef FANSHELL_QS_OXWMSTATE_H
#define FANSHELL_QS_OXWMSTATE_H

#include "oxwmipc.h"
#include "wmstate.h"

class OxwmState : public WmState {
    Q_OBJECT
public:
    explicit OxwmState(QObject *parent = nullptr);

    QString wmName() const override { return QStringLiteral("oxwm"); }
    Features features() const override;
    QString unavailableHint() const override;

    /* oxwm's tag actions are relative to its selected monitor, so each one
     * selects `monitorId` first. */
    void viewWorkspace(int monitorId, int index) override;
    void toggleWorkspace(int monitorId, int index) override;
    void cycleLayout(int monitorId) override;

    QVector<quintptr> windows() override;
    bool focusWindow(quintptr window) override;

protected:
    void refresh() override;

private:
    void applyTitle(const QString &data);

    OxwmEventStream m_events;
};

#endif
