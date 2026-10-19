/* The Control Panel: one centred window with a tile per settings window
 * (Displays, Screen power, Screen lock, Keyboard, Wallpaper), opened from the System
 * dropdown's header so the dropdown itself stays about status and quick
 * actions.
 *
 *     fanshell-qs-shell ipc call controlpanel toggle|show|hide
 *
 * Each tile shows a line of live status and opens its own window, which
 * replaces the panel on screen. Adding a setting means adding a tile here. */
#ifndef FANSHELL_QS_CONTROLPANEL_H
#define FANSHELL_QS_CONTROLPANEL_H

#include <QObject>

class ControlPanelWindow;

class ControlPanel : public QObject {
public:
    static ControlPanel *instance();

    void openWindow();
    void toggleWindow();
    void hideWindow();

private:
    explicit ControlPanel(QObject *parent = nullptr) : QObject(parent) {}

    ControlPanelWindow *m_win = nullptr;
};

#endif
