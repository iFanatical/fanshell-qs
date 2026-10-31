#include "wmstate.h"

#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QScreen>

#include <xcb/xcb.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef FANSHELL_QS_HAVE_WAYLAND
#include "hyprstate.h"
#endif
#include "oxwmstate.h"
#include "qtileipc.h"
#include "qtilestate.h"
#include "surface.h"

namespace {

bool sameMonitors(const QVector<WmState::Monitor> &a,
                  const QVector<WmState::Monitor> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        const auto &x = a[i];
        const auto &y = b[i];
        if (x.id != y.id || x.name != y.name || x.x != y.x || x.y != y.y
                || x.w != y.w || x.h != y.h || x.focused != y.focused
                || x.selected != y.selected || x.elsewhere != y.elsewhere
                || x.occupied != y.occupied || x.urgent != y.urgent
                || x.layout != y.layout || x.title != y.title
                || x.fullscreen != y.fullscreen || x.labels != y.labels)
            return false;
    }
    return true;
}

xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 1, uint16_t(strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

/* The running window manager's name, as it advertises it to EWMH clients:
 * _NET_WM_NAME on the window _NET_SUPPORTING_WM_CHECK names ("oxwm",
 * "qtile"). Empty when no window manager is up yet. */
QString ewmhName()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    xcb_connection_t *c = x11 ? x11->connection() : nullptr;
    /* On XWayland (a Wayland session) this is no window manager's. */
    if (!c)
        return QString();
    const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    const xcb_atom_t check = atom(c, "_NET_SUPPORTING_WM_CHECK");
    const xcb_atom_t name = atom(c, "_NET_WM_NAME");
    const xcb_atom_t utf8 = atom(c, "UTF8_STRING");
    if (!check || !name)
        return QString();
    xcb_window_t wm = XCB_WINDOW_NONE;
    if (auto *r = xcb_get_property_reply(
            c, xcb_get_property(c, 0, root, check, XCB_ATOM_WINDOW, 0, 1),
            nullptr)) {
        if (xcb_get_property_value_length(r) >= int(sizeof(xcb_window_t)))
            wm = *static_cast<xcb_window_t *>(xcb_get_property_value(r));
        free(r);
    }
    if (wm == XCB_WINDOW_NONE)
        return QString();
    QString out;
    if (auto *r = xcb_get_property_reply(
            c, xcb_get_property(c, 0, wm, name, utf8, 0, 64), nullptr)) {
        out = QString::fromUtf8(static_cast<const char *>(xcb_get_property_value(r)),
                                xcb_get_property_value_length(r));
        free(r);
    }
    return out;
}

/* Which backend to run: $FANSHELL_QS_WM if set, else the running window
 * manager's EWMH name, else $XDG_CURRENT_DESKTOP (set by .X_envars), else
 * qtile when its IPC socket for this display exists (qtile's stock config
 * calls itself "LG3D"), else oxwm, whose backend keeps retrying until its
 * socket appears. */
WmState *createBackend()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString wm = env.value(QStringLiteral("FANSHELL_QS_WM"));
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* Hyprland sets its signature in the environment of everything it
     * starts, and the shell only runs on Wayland under Hyprland. */
    if (wm.isEmpty() && (Surface::wayland()
                         || env.contains(QStringLiteral("HYPRLAND_INSTANCE_SIGNATURE"))))
        wm = QStringLiteral("hyprland");
    if (wm.compare(QLatin1String("hyprland"), Qt::CaseInsensitive) == 0)
        return new HyprState();
#endif
    if (wm.isEmpty())
        wm = ewmhName();
    if (wm.isEmpty())
        wm = env.value(QStringLiteral("XDG_CURRENT_DESKTOP"));
    wm = wm.toLower();
    if (wm.contains(QLatin1String("qtile")))
        return new QtileState();
    if (!wm.contains(QLatin1String("oxwm")) && QtileIpc::available())
        return new QtileState();
    return new OxwmState();
}

/* A QScreen's rectangle in device pixels, which is what Xinerama reports. */
QRect nativeGeometry(const QScreen *screen)
{
    const QRect g = screen->geometry();
    const qreal r = screen->devicePixelRatio();
    return QRect(qRound(g.x() * r), qRound(g.y() * r), qRound(g.width() * r),
                 qRound(g.height() * r));
}

} /* namespace */

WmState *WmState::instance()
{
    static WmState *s = createBackend();
    return s;
}

WmState::WmState(QObject *parent) : QObject(parent)
{
    /* Zero-delay single shot: collapses the burst of notifications one user
     * action produces into a single re-query, without adding latency. */
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(0);
    connect(&m_coalesce, &QTimer::timeout, this, [this]() { refresh(); });

    /* Monitor names come from Qt's screens, which can settle after the
     * window manager has already reported the new layout. */
    auto renamed = [this]() { scheduleRefresh(); };
    connect(qApp, &QGuiApplication::screenAdded, this, renamed);
    connect(qApp, &QGuiApplication::screenRemoved, this, renamed);
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, renamed);
}

const WmState::Monitor *WmState::monitorByName(const QString &name) const
{
    for (const Monitor &m : monitors)
        if (m.name == name)
            return &m;
    return nullptr;
}

QString WmState::primaryMonitor() const
{
    if (const QScreen *primary = QGuiApplication::primaryScreen())
        if (monitorByName(primary->name()))
            return primary->name();
    const Monitor *lowest = nullptr;
    for (const Monitor &m : monitors)
        if (!lowest || m.id < lowest->id)
            lowest = &m;
    if (lowest)
        return lowest->name;
    const QScreen *primary = QGuiApplication::primaryScreen();
    return primary ? primary->name() : QString();
}

QString WmState::focusedMonitor() const
{
    for (const Monitor &m : monitors)
        if (m.focused)
            return m.name;
    return primaryMonitor();
}

void WmState::toggleWorkspace(int, int) {}

void WmState::cycleLayout(int) {}

void WmState::resetSubmap() {}

bool WmState::focusedFullscreen() const
{
    const Monitor *m = monitorByName(focusedMonitor());
    return m && m->fullscreen;
}

/* X window managers don't track PIDs, so read _NET_WM_PID off each window
 * they manage. Only an unambiguous match is focused. */
bool WmState::focusPid(uint pid)
{
    if (!pid)
        return false;
    quintptr window = 0;
    int matches = 0;
    for (quintptr xid : windows()) {
        if (Surface::windowPid(xid) != pid)
            continue;
        window = xid;
        matches++;
    }
    return matches == 1 && focusWindow(window);
}

void WmState::scheduleRefresh()
{
    if (!m_coalesce.isActive())
        m_coalesce.start();
}

void WmState::publish(QVector<Monitor> mons, const QStringList &names)
{
    if (mons.isEmpty() && !monitors.isEmpty())
        return;
    nameMonitors(mons);
    const bool same = connected && workspaceNames == names
                      && sameMonitors(monitors, mons);
    connected = true;
    monitors = mons;
    workspaceNames = names;
    if (!same)
        emit changed();
}

void WmState::setDisconnected()
{
    if (!connected)
        return;
    connected = false;
    emit changed();
}

bool WmState::updateTitle(int monitorId, const QString &title)
{
    for (Monitor &m : monitors) {
        if (m.id != monitorId)
            continue;
        if (m.title != title) {
            m.title = title;
            emit titleChanged(m.name);
        }
        return true;
    }
    return false;
}

void WmState::nameMonitors(QVector<Monitor> &mons) const
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (Monitor &m : mons) {
        /* A backend that knows the outputs by name already (Hyprland, whose
         * names are the ones Qt's Wayland screens carry) keeps them. */
        if (!m.name.isEmpty()
                && std::any_of(screens.begin(), screens.end(),
                               [&m](const QScreen *s) { return s->name() == m.name; }))
            continue;
        const QRect want(m.x, m.y, m.w, m.h);
        const QScreen *match = nullptr;
        for (const QScreen *s : screens) {
            const QRect g = nativeGeometry(s);
            if (g == want) {
                match = s;
                break;
            }
            /* Mid-hotplug Qt and the window manager can briefly disagree on
             * size; the origin is still unambiguous. */
            if (!match && g.topLeft() == want.topLeft())
                match = s;
        }
        m.name = match ? match->name()
                       : QStringLiteral("%1-%2").arg(wmName()).arg(m.id);
    }
}
