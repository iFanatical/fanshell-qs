/* fanshell-qs-shell: a hand-rolled C++/Qt bar for oxwm, qtile and Hyprland. One
 * process runs the per-output panels, popups, SNI tray, and launcher.
 *
 *   fanshell-qs-shell [--no-duplicate]        run the shell
 *   fanshell-qs-shell ipc call <target> <fn>  poke a running instance
 *                    (targets: shell, launcher, runner, emoji, notifications,
 *                     audio, brightness, media, vpn, wallpaper, menu,
 *                     outputs, dpms, lock, nightlight, keyboard, mouse,
 *                     controlpanel)
 *   fanshell-qs-shell lock                    lock the screen (lockscreen.h) */
#include <QApplication>
#include <QHash>
#include <QScreen>

#include "wmstate.h"
#include "capture.h"
#include "controlpanel.h"
#include "displays.h"
#include "gamemode.h"
#include "hardwarecontrols.h"
#include "idleinhibit.h"
#include "ipc.h"
#include "keyboard.h"
#include "mouse.h"
#include "launcher.h"
#include "lockscreen.h"
#include "media.h"
#include "nightlight.h"
#include "notification.h"
#include "osd.h"
#include "panel.h"
#include "popup.h"
#include "status.h"
#include "vibrance.h"
#include "wallpaper.h"
#include "pills.h"
#include "surface.h"
#ifdef FANSHELL_QS_HAVE_XEMBED
#include "xembedtray.h"
#endif

int main(int argc, char *argv[])
{
    /* Client mode must not need a display connection. */
    QStringList rawArgs;
    for (int i = 1; i < argc; i++)
        rawArgs << QString::fromLocal8Bit(argv[i]);
    if (!rawArgs.isEmpty() && rawArgs[0] == QLatin1String("ipc")) {
        QStringList a = rawArgs.mid(1);
        if (!a.isEmpty() && a[0] == QLatin1String("call"))
            a.removeFirst();
        if (a.size() < 2) {
            fprintf(stderr,
                    "usage: fanshell-qs-shell ipc call <target> <function>\n");
            return 2;
        }
        return IpcServer::call(a[0], a[1]);
    }
    /* Its own processes, with their own QApplication. */
    if (!rawArgs.isEmpty() && rawArgs[0] == QLatin1String("lock"))
        return LockScreen::run(argc, argv);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fanshell-qs-shell"));
    app.setQuitOnLastWindowClosed(false);

    if (rawArgs.contains(QLatin1String("--no-duplicate"))
            && IpcServer::serverRunning())
        return 0;

    if (!Surface::active())
        fprintf(stderr,
                "fanshell-qs-shell: platform is '%s', not xcb%s — the bar "
                "will come up as an ordinary window with no reserved strip. "
                "Force it with QT_QPA_PLATFORM=xcb%s.\n",
                qPrintable(QGuiApplication::platformName()),
#ifdef FANSHELL_QS_HAVE_WAYLAND
                " or wayland", " (or wayland, with qtwayland installed)"
#else
                " (a build without Wayland support)", ""
#endif
                );
    WmState *state = WmState::instance();
    if (const QString hint = state->unavailableHint(); !hint.isEmpty())
        fprintf(stderr,
                "fanshell-qs-shell: %s Workspaces and the window title stay "
                "empty until it appears.\n",
                qPrintable(hint));
    if (!NotificationService::instance()->start())
        fprintf(stderr,
                "fanshell-qs-shell: cannot own org.freedesktop.Notifications "
                "-- stop dunst or another notification daemon first.\n");

    /* One panel per output, following hotplug. */
    QHash<QScreen *, Panel *> panels;
    auto addPanel = [&panels, state](QScreen *s) {
        if (panels.contains(s))
            return;
        auto *p = new Panel(state, s);
        panels.insert(s, p);
        p->show();
    };
    const QList<QScreen *> screens = app.screens();
    for (QScreen *s : screens)
        addPanel(s);
#ifdef FANSHELL_QS_HAVE_XEMBED
    /* The XEmbed tray: owns _NET_SYSTEM_TRAY_S<screen> for legacy X11
     * tray icons, alongside the StatusNotifierItem tray. */
    XEmbedTray::instance();
#endif
    QObject::connect(&app, &QGuiApplication::screenAdded, &app, addPanel);
    QObject::connect(&app, &QGuiApplication::screenRemoved, &app,
                     [&panels](QScreen *s) {
                         if (Panel *p = panels.take(s))
                             p->deleteLater();
                     });

    /* Single centered launcher, opened on the currently focused output. */
    auto *launcher = new AppLauncher(&app);

    /* Built now rather than on first use: it scans the wallpaper directory
     * and reads wallpaper.conf for what is displayed, and it decodes its
     * thumbnails one per event-loop turn. Left lazy, the first open would
     * race its own query and pay the whole decode cost on the spot.
     * Construction also re-applies the saved wallpaper. */
    WallpaperPicker::instance();
    /* Re-applies the saved digital vibrance; see vibrance.h. */
    Vibrance::instance();
    /* Watches RandR, to apply the saved layout when a monitor is plugged in
     * or removed; the Displays popup itself is built on first open. */
    DisplaySettings::instance();
    /* Applies the saved blanking and DPMS settings. */
    ScreenPower::instance();
    /* Takes programs' requests not to blank or lock (a video, a call);
     * before the lock, which honours them. */
    IdleInhibitor::instance();
    /* Locks on idle and before sleep, per lock.conf. */
    ScreenLock *lock = ScreenLock::instance();
    /* Warms the colours after sunset, per nightlight.conf. */
    NightLight::instance();
    /* Turns Num Lock on, once per X session, per keyboard.conf. */
    KeyboardSettings::instance();
    /* Applies the saved pointer settings, and again on hotplug. */
    MouseSettings::instance();

    /* The lock screen grabs the keyboard, so what holds it goes first; and
     * nothing of the shell's shows over it. */
    QObject::connect(lock, &ScreenLock::aboutToLock, launcher, [launcher]() {
        launcher->hideLauncher();
        WallpaperPicker::instance()->hidePicker();
        PopupManager::closeCurrent();
        ControlPanel::instance()->hideWindow();
        DisplaySettings::instance()->hideWindow();
        ScreenPower::instance()->hideWindow();
        ScreenLock::instance()->hideWindow();
        NightLight::instance()->hideWindow();
        KeyboardSettings::instance()->hideWindow();
        MouseSettings::instance()->hideWindow();
    });
    auto onLocked = [](bool locked) {
        NotificationService::instance()->setLocked(locked);
        HardwareOsd::setSuppressed(locked);
    };
    QObject::connect(lock, &ScreenLock::lockedChanged, &app, onLocked);
    if (lock->locked())
        onLocked(true);

    auto *ipc = new IpcServer(&app);
    ipc->handle(QStringLiteral("menu"), QStringLiteral("toggle"),
                [launcher]() { launcher->toggleMode(QStringLiteral("menu")); });
    ipc->handle(QStringLiteral("menu"), QStringLiteral("show"),
                [launcher]() { launcher->openMode(QStringLiteral("menu")); });
    ipc->handle(QStringLiteral("menu"), QStringLiteral("hide"),
                [launcher]() {
                    if (launcher->mode == QLatin1String("menu"))
                        launcher->hideLauncher();
                });
    HardwareControls *hardware = HardwareControls::instance();
    MediaControls *media = MediaControls::instance();
    ipc->handle(QStringLiteral("shell"), QStringLiteral("quit"),
                []() { QCoreApplication::quit(); });
    ipc->handle(QStringLiteral("wm"), QStringLiteral("changed"),
                [state]() { state->wmChanged(); });
    ipc->handle(QStringLiteral("launcher"), QStringLiteral("toggle"),
                [launcher]() { launcher->toggleMode(QStringLiteral("apps")); });
    ipc->handle(QStringLiteral("launcher"), QStringLiteral("show"),
                [launcher]() { launcher->openMode(QStringLiteral("apps")); });
    ipc->handle(QStringLiteral("launcher"), QStringLiteral("hide"),
                [launcher]() { launcher->hideLauncher(); });
    ipc->handle(QStringLiteral("runner"), QStringLiteral("toggle"),
                [launcher]() { launcher->toggleMode(QStringLiteral("run")); });
    ipc->handle(QStringLiteral("runner"), QStringLiteral("show"),
                [launcher]() { launcher->openMode(QStringLiteral("run")); });
    ipc->handle(QStringLiteral("runner"), QStringLiteral("hide"),
                [launcher]() { launcher->hideLauncher(); });
    ipc->handle(QStringLiteral("emoji"), QStringLiteral("toggle"),
                [launcher]() { launcher->toggleMode(QStringLiteral("emoji")); });
    ipc->handle(QStringLiteral("emoji"), QStringLiteral("show"),
                [launcher]() { launcher->openMode(QStringLiteral("emoji")); });
    ipc->handle(QStringLiteral("emoji"), QStringLiteral("hide"),
                [launcher]() { launcher->hideLauncher(); });
    ipc->handle(QStringLiteral("notifications"), QStringLiteral("toggle-dnd"),
                []() { NotificationService::instance()->toggleDnd(); });
    ipc->handle(QStringLiteral("audio"), QStringLiteral("volume-up"),
                [hardware]() { hardware->volumeUp(); });
    ipc->handle(QStringLiteral("audio"), QStringLiteral("volume-down"),
                [hardware]() { hardware->volumeDown(); });
    ipc->handle(QStringLiteral("audio"), QStringLiteral("toggle-mute"),
                [hardware]() { hardware->toggleVolumeMute(); });
    ipc->handle(QStringLiteral("audio"), QStringLiteral("toggle-mic"),
                [hardware]() { hardware->toggleMicrophoneMute(); });
    ipc->handle(QStringLiteral("media"), QStringLiteral("play-pause"),
                [media]() { media->playPause(); });
    ipc->handle(QStringLiteral("media"), QStringLiteral("previous"),
                [media]() { media->previous(); });
    ipc->handle(QStringLiteral("media"), QStringLiteral("next"),
                [media]() { media->next(); });
    ipc->handle(QStringLiteral("brightness"), QStringLiteral("up"),
                [hardware]() { hardware->brightnessUp(); });
    ipc->handle(QStringLiteral("brightness"), QStringLiteral("down"),
                [hardware]() { hardware->brightnessDown(); });
    ipc->handle(QStringLiteral("brightness"), QStringLiteral("monitor-up"),
                [hardware]() { hardware->monitorBrightnessUp(); });
    ipc->handle(QStringLiteral("brightness"), QStringLiteral("monitor-down"),
                [hardware]() { hardware->monitorBrightnessDown(); });
    if (hardware->hasKeyboardBacklight()) {
        ipc->handle(QStringLiteral("brightness"),
                    QStringLiteral("keyboard-up"),
                    [hardware]() { hardware->keyboardBacklightUp(); });
        ipc->handle(QStringLiteral("brightness"),
                    QStringLiteral("keyboard-down"),
                    [hardware]() { hardware->keyboardBacklightDown(); });
        ipc->handle(QStringLiteral("brightness"),
                    QStringLiteral("keyboard-cycle"),
                    [hardware]() { hardware->keyboardBacklightCycle(); });
    }
    ipc->handle(QStringLiteral("vpn"), QStringLiteral("toggle"),
                []() { VpnState::instance()->toggle(); });
    for (const QString &tunnel : VpnState::instance()->tunnels())
        ipc->handle(QStringLiteral("vpn"), tunnel,
                    [tunnel]() { VpnState::instance()->select(tunnel); });
    ipc->handle(QStringLiteral("vpn"), QStringLiteral("refresh"),
                []() { VpnState::instance()->refresh(); });
    ipc->handle(QStringLiteral("outputs"), QStringLiteral("toggle"),
                []() { DisplaySettings::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("outputs"), QStringLiteral("show"),
                []() { DisplaySettings::instance()->openWindow(); });
    ipc->handle(QStringLiteral("outputs"), QStringLiteral("hide"),
                []() { DisplaySettings::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("dpms"), QStringLiteral("toggle"),
                []() { ScreenPower::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("dpms"), QStringLiteral("show"),
                []() { ScreenPower::instance()->openWindow(); });
    ipc->handle(QStringLiteral("dpms"), QStringLiteral("hide"),
                []() { ScreenPower::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("dpms"), QStringLiteral("off"),
                []() { ScreenPower::instance()->turnOffNow(); });
    ipc->handle(QStringLiteral("controlpanel"), QStringLiteral("toggle"),
                []() { ControlPanel::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("controlpanel"), QStringLiteral("show"),
                []() { ControlPanel::instance()->openWindow(); });
    ipc->handle(QStringLiteral("controlpanel"), QStringLiteral("hide"),
                []() { ControlPanel::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("keyboard"), QStringLiteral("toggle"),
                []() { KeyboardSettings::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("keyboard"), QStringLiteral("show"),
                []() { KeyboardSettings::instance()->openWindow(); });
    ipc->handle(QStringLiteral("keyboard"), QStringLiteral("hide"),
                []() { KeyboardSettings::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("mouse"), QStringLiteral("toggle"),
                []() { MouseSettings::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("mouse"), QStringLiteral("show"),
                []() { MouseSettings::instance()->openWindow(); });
    ipc->handle(QStringLiteral("mouse"), QStringLiteral("hide"),
                []() { MouseSettings::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("lock"), QStringLiteral("toggle"),
                []() { ScreenLock::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("lock"), QStringLiteral("show"),
                []() { ScreenLock::instance()->openWindow(); });
    ipc->handle(QStringLiteral("lock"), QStringLiteral("hide"),
                []() { ScreenLock::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("lock"), QStringLiteral("now"),
                []() { ScreenLock::instance()->lockNow(); });
    /* From the lock screen itself. */
    ipc->handle(QStringLiteral("lock"), QStringLiteral("locked"),
                []() { ScreenLock::instance()->setLocked(true); });
    ipc->handle(QStringLiteral("lock"), QStringLiteral("unlocked"),
                []() { ScreenLock::instance()->setLocked(false); });
    ipc->handle(QStringLiteral("nightlight"), QStringLiteral("toggle"),
                []() { NightLight::instance()->toggleWindow(); });
    ipc->handle(QStringLiteral("nightlight"), QStringLiteral("show"),
                []() { NightLight::instance()->openWindow(); });
    ipc->handle(QStringLiteral("nightlight"), QStringLiteral("hide"),
                []() { NightLight::instance()->hideWindow(); });
    ipc->handle(QStringLiteral("nightlight"), QStringLiteral("enable"),
                []() { NightLight::instance()->setEnabled(true); });
    ipc->handle(QStringLiteral("nightlight"), QStringLiteral("disable"),
                []() { NightLight::instance()->setEnabled(false); });
    ipc->handle(QStringLiteral("nightlight"), QStringLiteral("toggle-enabled"), []() {
        NightLight *n = NightLight::instance();
        n->setEnabled(!n->settings().enabled);
    });
    ipc->handle(QStringLiteral("wallpaper"), QStringLiteral("toggle"),
                []() { WallpaperPicker::instance()->togglePicker(); });
    ipc->handle(QStringLiteral("wallpaper"), QStringLiteral("show"),
                []() { WallpaperPicker::instance()->openPicker(); });
    ipc->handle(QStringLiteral("wallpaper"), QStringLiteral("hide"),
                []() { WallpaperPicker::instance()->hidePicker(); });
    ipc->handle(QStringLiteral("keepawake"), QStringLiteral("toggle"), []() {
        IdleInhibitor *i = IdleInhibitor::instance();
        i->setKeepAwake(!i->keepAwake());
    });
    ipc->handle(QStringLiteral("keepawake"), QStringLiteral("on"),
                []() { IdleInhibitor::instance()->setKeepAwake(true); });
    ipc->handle(QStringLiteral("keepawake"), QStringLiteral("off"),
                []() { IdleInhibitor::instance()->setKeepAwake(false); });
    ipc->handle(QStringLiteral("gamemode"), QStringLiteral("toggle"),
                []() { GameMode::instance()->toggle(); });
    ipc->handle(QStringLiteral("gamemode"), QStringLiteral("on"),
                []() { GameMode::instance()->setActive(true); });
    ipc->handle(QStringLiteral("gamemode"), QStringLiteral("off"),
                []() { GameMode::instance()->setActive(false); });
    ipc->handle(QStringLiteral("picker"), QStringLiteral("pick"),
                []() { Capture::instance()->pickColor(); });
    ipc->handle(QStringLiteral("screenshot"), QStringLiteral("region"),
                []() { Capture::instance()->screenshot(Capture::Kind::Region); });
    ipc->handle(QStringLiteral("screenshot"), QStringLiteral("output"),
                []() { Capture::instance()->screenshot(Capture::Kind::Output); });
    ipc->handle(QStringLiteral("screenshot"), QStringLiteral("screen"),
                []() { Capture::instance()->screenshot(Capture::Kind::Screen); });
    if (!ipc->listen())
        fprintf(stderr,
                "fanshell-qs-shell: cannot listen on %s -- another instance "
                "may own it; `ipc call` will reach that one, not this one. "
                "Start with --no-duplicate to avoid running twice.\n",
                qPrintable(IpcServer::socketPath()));

    return app.exec();
}
