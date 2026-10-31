#include "gamemode.h"

#include <QGuiApplication>

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

#include "idleinhibit.h"
#include "nightlight.h"
#include "notification.h"
#include "paths.h"
#include "powerprofiles.h"
#include "surface.h"
#include "wmstate.h"
#ifdef FANSHELL_QS_HAVE_WAYLAND
#include "hypripc.h"
#endif

namespace {

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 1, uint16_t(strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

/* Whether the focused window is fullscreen: _NET_ACTIVE_WINDOW's state on
 * X11, the window manager's word for it elsewhere. */
bool fullscreenFocused()
{
    xcb_connection_t *c = conn();
    if (!c)
        return WmState::instance()->focusedFullscreen();
    const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    const xcb_atom_t active = atom(c, "_NET_ACTIVE_WINDOW");
    const xcb_atom_t state = atom(c, "_NET_WM_STATE");
    const xcb_atom_t full = atom(c, "_NET_WM_STATE_FULLSCREEN");
    if (!active || !state || !full)
        return false;
    xcb_window_t window = XCB_WINDOW_NONE;
    if (auto *r = xcb_get_property_reply(
            c, xcb_get_property(c, 0, root, active, XCB_ATOM_WINDOW, 0, 1), nullptr)) {
        if (xcb_get_property_value_length(r) >= int(sizeof(xcb_window_t)))
            window = *static_cast<xcb_window_t *>(xcb_get_property_value(r));
        free(r);
    }
    if (window == XCB_WINDOW_NONE)
        return false;
    bool fullscreen = false;
    if (auto *r = xcb_get_property_reply(
            c, xcb_get_property(c, 0, window, state, XCB_ATOM_ATOM, 0, 32), nullptr)) {
        const auto *atoms = static_cast<xcb_atom_t *>(xcb_get_property_value(r));
        const int n = xcb_get_property_value_length(r) / int(sizeof(xcb_atom_t));
        for (int i = 0; i < n; i++)
            fullscreen = fullscreen || atoms[i] == full;
        free(r);
    }
    return fullscreen;
}

} /* namespace */

GameMode *GameMode::instance()
{
    static GameMode *s = new GameMode();
    return s;
}

GameMode::GameMode(QObject *parent) : QObject(parent)
{
    m_watch.setInterval(2000);
    connect(&m_watch, &QTimer::timeout, this, &GameMode::watchFullscreen);
    m_watch.start();
    /* Leave things as they were for whatever runs after the shell. */
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { enable(false); });
}

void GameMode::setActive(bool on)
{
    /* By hand while a fullscreen window turned it on: stay off until that
     * window is gone. */
    if (!on && m_autoOn)
        m_suppressed = true;
    m_autoOn = false;
    enable(on);
}

void GameMode::enable(bool on)
{
    if (on == m_active)
        return;
    m_active = on;
    NotificationService *notes = NotificationService::instance();
    PowerProfileState *power = PowerProfileState::instance();
    if (on) {
        m_dndBefore = notes->dnd();
        notes->setDnd(true);
        m_profileBefore = power->activeProfile();
        if (power->available() && power->profiles().contains(QLatin1String("performance")))
            power->setProfile(QStringLiteral("performance"));
    } else {
        notes->setDnd(m_dndBefore);
        if (power->available() && !m_profileBefore.isEmpty()
                && power->activeProfile() == QLatin1String("performance"))
            power->setProfile(m_profileBefore);
    }
#ifdef FANSHELL_QS_HAVE_WAYLAND
    if (Surface::wayland())
        setCompositorEffects(!on);
#endif
    NightLight::instance()->setHeld(on);
    IdleInhibitor::instance()->setShellRequest(IdleInhibitor::gameModeCookie,
                                               QStringLiteral("Game mode"), on);
    emit changed();
}

#ifdef FANSHELL_QS_HAVE_WAYLAND
/* Hyprland's animations, blur and shadows cost frames; off while game mode
 * is on, and back to what they were after. */
void GameMode::setCompositorEffects(bool on)
{
    static const char *const options[] = {"animations:enabled",
                                          "decoration:blur:enabled",
                                          "decoration:shadow:enabled"};
    if (!on) {
        m_effectsBefore.clear();
        for (const char *o : options) {
            const QVariant v = HyprIpc::optionValue(QLatin1String(o));
            if (!v.isValid())
                continue;
            m_effectsBefore.insert(QLatin1String(o), v.toInt() != 0);
            HyprIpc::setOption(QLatin1String(o), false);
        }
        return;
    }
    for (auto it = m_effectsBefore.constBegin(); it != m_effectsBefore.constEnd(); ++it)
        if (it.value())
            HyprIpc::setOption(it.key(), true);
    m_effectsBefore.clear();
}
#endif

void GameMode::watchFullscreen()
{
    if (Paths::setting(QStringLiteral("gamemode.auto")).toLower()
            != QLatin1String("fullscreen")) {
        if (m_autoOn) {
            m_autoOn = false;
            enable(false);
        }
        return;
    }
    const bool full = fullscreenFocused();
    if (!full) {
        m_suppressed = false;
        if (m_autoOn) {
            m_autoOn = false;
            enable(false);
        }
    } else if (!m_active && !m_suppressed) {
        m_autoOn = true;
        enable(true);
    }
}
