#include "surface.h"

#include <QGuiApplication>
#include <QHash>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <xcb/xcb.h>

#include <cstdlib>

/* Everything here talks to the X server through xcb on Qt's own connection.
 * Xlib on Qt's Display would also work, but an Xlib error (focusing a window
 * that just died, say) goes to Xlib's default handler, which exits the
 * process; an xcb error just becomes a warning on Qt's event queue. */

namespace {

struct Entry {
    QPointer<QScreen> screen;
    Surface::SurfaceSpec spec;
    QPoint origin;            /* output-local top-left we placed it at */
    int corrections = 0;      /* keepDockInPlace() moves since it last fit */
    xcb_window_t prevFocus = XCB_NONE;
    bool grabbed = false;
    int grabTries = 0;
};

QHash<const QWidget *, Entry> g_entries;

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

xcb_atom_t atom(const char *name)
{
    static QHash<QByteArray, xcb_atom_t> cache;
    const QByteArray key(name);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    xcb_connection_t *c = conn();
    if (!c)
        return XCB_NONE;
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 0, key.size(), key.constData()), nullptr);
    const xcb_atom_t a = reply ? reply->atom : XCB_NONE;
    free(reply);
    cache.insert(key, a);
    return a;
}

void setAtoms(xcb_window_t w, const char *property,
              std::initializer_list<const char *> values)
{
    QVector<xcb_atom_t> atoms;
    for (const char *v : values)
        atoms.push_back(atom(v));
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom(property),
                        XCB_ATOM_ATOM, 32, atoms.size(), atoms.constData());
}

void setCardinals(xcb_window_t w, const char *property,
                  const QVector<quint32> &values)
{
    xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, w, atom(property),
                        XCB_ATOM_CARDINAL, 32, values.size(),
                        values.constData());
}

const char *typeAtom(Surface::WindowType t, bool dock)
{
    switch (t) {
    case Surface::WindowType::Dock:
        return "_NET_WM_WINDOW_TYPE_DOCK";
    case Surface::WindowType::PopupMenu:
        return "_NET_WM_WINDOW_TYPE_POPUP_MENU";
    case Surface::WindowType::Notification:
        return "_NET_WM_WINDOW_TYPE_NOTIFICATION";
    case Surface::WindowType::Utility:
        return "_NET_WM_WINDOW_TYPE_UTILITY";
    case Surface::WindowType::Dialog:
        return "_NET_WM_WINDOW_TYPE_DIALOG";
    case Surface::WindowType::Auto:
        break;
    }
    return dock ? "_NET_WM_WINDOW_TYPE_DOCK" : "_NET_WM_WINDOW_TYPE_POPUP_MENU";
}

bool isDock(const Surface::SurfaceSpec &spec)
{
    return spec.exclusiveZone > 0;
}

/* Strip reserved by docks we own on `screen`, so surfaces with
 * exclusiveZone 0 can keep clear of the bar like they would of a layer-shell
 * exclusive zone. */
QMargins reservedOn(const QScreen *screen, const QWidget *except)
{
    QMargins r;
    for (auto it = g_entries.constBegin(); it != g_entries.constEnd(); ++it) {
        if (it.key() == except || it->screen != screen || !isDock(it->spec))
            continue;
        if (!it.key()->isVisible())
            continue;
        const int a = it->spec.anchors;
        const int zone = it->spec.exclusiveZone;
        if ((a & Surface::AnchorTop) && !(a & Surface::AnchorBottom))
            r.setTop(qMax(r.top(), zone));
        else if ((a & Surface::AnchorBottom) && !(a & Surface::AnchorTop))
            r.setBottom(qMax(r.bottom(), zone));
    }
    return r;
}

/* Lay one axis out the way layer shell does. */
void place(int areaStart, int areaLen, bool startAnchor, bool endAnchor,
           int startMargin, int endMargin, int desired, int current,
           int &pos, int &len)
{
    if (startAnchor && endAnchor) {
        len = desired > 0 ? desired : areaLen - startMargin - endMargin;
        /* Both anchored with an explicit size: centred between the margins. */
        pos = areaStart + startMargin
              + (areaLen - startMargin - endMargin - len) / 2;
    } else {
        len = desired > 0 ? desired : current;
        if (startAnchor)
            pos = areaStart + startMargin;
        else if (endAnchor)
            pos = areaStart + areaLen - endMargin - len;
        else
            pos = areaStart + (areaLen - len) / 2;
    }
    len = qMax(1, len);
}

void applyGeometry(QWidget *w)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end())
        return;
    QScreen *screen = it->screen ? it->screen.data() : w->screen();
    if (!screen)
        return;

    const Surface::SurfaceSpec &spec = it->spec;
    const QRect full = screen->geometry();
    QRect area = full;
    if (spec.exclusiveZone == 0)
        area = full.marginsRemoved(reservedOn(screen, w));

    int x, y, width, height;
    place(area.x(), area.width(), spec.anchors & Surface::AnchorLeft,
          spec.anchors & Surface::AnchorRight, spec.margins.left(),
          spec.margins.right(), spec.desiredSize.width(), w->width(), x,
          width);
    place(area.y(), area.height(), spec.anchors & Surface::AnchorTop,
          spec.anchors & Surface::AnchorBottom, spec.margins.top(),
          spec.margins.bottom(), spec.desiredSize.height(), w->height(), y,
          height);

    it->origin = QPoint(x, y) - full.topLeft();
    w->move(x, y);
    w->resize(width, height);
}

/* _NET_WM_STRUT(_PARTIAL) for docks, in root-window device pixels. Read by
 * any EWMH window manager (qtile); the patched oxwm uses the dock's
 * geometry directly, which comes to the same thing. */
void applyStrut(QWidget *w, const Entry &e)
{
    if (!isDock(e.spec) || !e.screen)
        return;
    const qreal dpr = e.screen->devicePixelRatio();
    const QRect g = e.screen->geometry();
    const int x0 = qRound(g.x() * dpr);
    const int x1 = qRound((g.x() + g.width()) * dpr) - 1;
    const int zone = qRound(e.spec.exclusiveZone * dpr);
    QVector<quint32> partial(12, 0);
    if (e.spec.anchors & Surface::AnchorBottom
            && !(e.spec.anchors & Surface::AnchorTop)) {
        const xcb_screen_t *root =
            xcb_setup_roots_iterator(xcb_get_setup(conn())).data;
        const int bottomGap = root->height_in_pixels
                              - qRound((g.y() + g.height()) * dpr);
        partial[3] = quint32(zone + bottomGap);
        partial[10] = quint32(x0);
        partial[11] = quint32(x1);
    } else {
        partial[2] = quint32(qRound(g.y() * dpr) + zone);
        partial[8] = quint32(x0);
        partial[9] = quint32(x1);
    }
    const xcb_window_t win = xcb_window_t(w->winId());
    setCardinals(win, "_NET_WM_STRUT_PARTIAL", partial);
    setCardinals(win, "_NET_WM_STRUT", partial.mid(0, 4));
}

xcb_window_t rootWindow()
{
    return xcb_setup_roots_iterator(xcb_get_setup(conn())).data->root;
}

xcb_window_t inputFocus()
{
    xcb_connection_t *c = conn();
    xcb_get_input_focus_reply_t *reply =
        xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
    const xcb_window_t w = reply ? reply->focus : XCB_NONE;
    free(reply);
    return w;
}

const QWidget *ownWidget(xcb_window_t w)
{
    for (auto it = g_entries.constBegin(); it != g_entries.constEnd(); ++it)
        if (it.key()->internalWinId() == w)
            return it.key();
    return nullptr;
}

void tryGrab(QWidget *w)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end() || !w->isVisible() || it->grabbed)
        return;
    xcb_connection_t *c = conn();
    xcb_grab_keyboard_reply_t *reply = xcb_grab_keyboard_reply(
        c,
        xcb_grab_keyboard(c, 1, xcb_window_t(w->winId()), XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
        nullptr);
    const bool ok = reply && reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(reply);
    if (ok) {
        it->grabbed = true;
        return;
    }
    /* The key that summoned us may still be held under the WM's passive
     * grab, or the window not yet viewable; dmenu retries for the same
     * reasons. */
    if (++it->grabTries < 100) {
        QPointer<QWidget> guard(w);
        QTimer::singleShot(10, w, [guard]() {
            if (guard)
                tryGrab(guard);
        });
    }
}

void onShown(QWidget *w)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end())
        return;
    if (!isDock(it->spec))
        w->raise();
    const Surface::Keyboard k = it->spec.keyboard;
    if (k == Surface::Keyboard::None || !it->spec.activateOnShow)
        return;
    /* Remember who had focus -- a client, or a popup this one opens over (a
     * submenu) -- so hiding can hand it back. */
    const xcb_window_t prev = inputFocus();
    if (prev != xcb_window_t(w->internalWinId()))
        it->prevFocus = prev;
    w->activateWindow();
    if (k == Surface::Keyboard::Exclusive) {
        it->grabTries = 0;
        tryGrab(w);
    }
}

void onHidden(QWidget *w)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end())
        return;
    xcb_connection_t *c = conn();
    if (it->grabbed) {
        xcb_ungrab_keyboard(c, XCB_CURRENT_TIME);
        it->grabbed = false;
    }
    it->grabTries = 100; /* stop any pending retry */
    const xcb_window_t prev = it->prevFocus;
    it->prevFocus = XCB_NONE;
    /* Only hand focus back if it is still ours: if the user has moved on to
     * another window meanwhile, leave it there. Unmapping our window has
     * usually already reverted focus to the root (Qt maps with
     * RevertToParent), which counts as nobody's. A previous holder that is
     * one of our own surfaces only gets it back while it is still shown. */
    const xcb_window_t now = inputFocus();
    const xcb_window_t root = rootWindow();
    const QWidget *prevOwn = ownWidget(prev);
    if (prev != XCB_NONE && prev != XCB_INPUT_FOCUS_POINTER_ROOT && prev != root
            && (!prevOwn || prevOwn->isVisible())
            && (now == XCB_NONE || now == XCB_INPUT_FOCUS_POINTER_ROOT
                || now == root || now == xcb_window_t(w->internalWinId())
                || ownWidget(now))) {
        xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, prev,
                            XCB_CURRENT_TIME);
    }
    xcb_flush(c);
}

/* Some window managers move a dock after mapping it: qtile 0.37 attaches it
 * to the focused screen and adds that screen's offset to the dock's already
 * absolute position, so with the focused monitor at x=1920 the panels land
 * one screen to the right and one ends up off the desktop. Put the dock back
 * where applyGeometry() placed it. This goes straight to X, not through
 * QWidget::move(), whose cached geometry may already read as right; the WM
 * honours a dock's ConfigureRequest (qtile does, for a dock attached without
 * a fixed position). Bounded, so a WM that insists on its own placement
 * cannot start a configure loop; a no-op where the WM leaves docks alone
 * (oxwm). */
void keepDockInPlace(QWidget *w)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end() || !isDock(it->spec) || !it->screen
            || !w->isVisible())
        return;
    const QPoint want = it->screen->geometry().topLeft() + it->origin;
    if (w->pos() == want) {
        it->corrections = 0;
        return;
    }
    if (it->corrections >= 3)
        return;
    ++it->corrections;
    const qreal dpr = it->screen->devicePixelRatio();
    const uint32_t xy[] = {uint32_t(qRound(want.x() * dpr)),
                           uint32_t(qRound(want.y() * dpr))};
    xcb_configure_window(conn(), xcb_window_t(w->winId()),
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
    xcb_flush(conn());
}

class ShowWatcher : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        auto *w = static_cast<QWidget *>(o);
        if (e->type() == QEvent::Show) {
            /* QEvent::Show precedes the map; raise and focus after it. */
            QPointer<QWidget> guard(w);
            QTimer::singleShot(0, w, [guard]() {
                if (guard && guard->isVisible()) {
                    onShown(guard);
                    keepDockInPlace(guard);
                }
            });
        } else if (e->type() == QEvent::Move) {
            /* After the event: the WM's move is what Qt is reporting. */
            QPointer<QWidget> guard(w);
            QTimer::singleShot(0, w, [guard]() {
                if (guard)
                    keepDockInPlace(guard);
            });
        } else if (e->type() == QEvent::Hide) {
            onHidden(w);
        }
        return false;
    }
};

ShowWatcher *watcher()
{
    static ShowWatcher *w = new ShowWatcher(qApp);
    return w;
}

} /* namespace */

namespace Surface {

bool active()
{
    return QGuiApplication::platformName() == QLatin1String("xcb")
           && conn() != nullptr;
}

void configure(QWidget *w, QScreen *screen, const SurfaceSpec &spec)
{
    if (!w)
        return;

    const bool firstTime = !g_entries.contains(w);
    Entry &e = g_entries[w];
    e.screen = screen;
    e.spec = spec;
    if (firstTime) {
        QObject::connect(w, &QObject::destroyed, qApp,
                         [w]() { g_entries.remove(w); });
        w->installEventFilter(watcher());
    }

    if (!active()) {
        applyGeometry(w);
        return;
    }

    const bool dock = isDock(spec);
    Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint;
    if (!dock)
        flags |= Qt::X11BypassWindowManagerHint | Qt::WindowStaysOnTopHint;
    if (spec.keyboard == Keyboard::None)
        flags |= Qt::WindowDoesNotAcceptFocus;
    if (w->windowFlags() != flags)
        w->setWindowFlags(flags);
    w->setAttribute(Qt::WA_ShowWithoutActivating,
                    spec.keyboard == Keyboard::None || !spec.activateOnShow);
    if (w->windowTitle().isEmpty())
        w->setWindowTitle(spec.scope);
    if (screen)
        w->setScreen(screen);

    applyGeometry(w);

    /* Properties must be on the window before it maps: the WM decides dock
     * vs client from the MapRequest. */
    const xcb_window_t win = xcb_window_t(w->winId());
    setAtoms(win, "_NET_WM_WINDOW_TYPE", {typeAtom(spec.type, dock)});
    if (dock) {
        setAtoms(win, "_NET_WM_STATE",
                 {"_NET_WM_STATE_ABOVE", "_NET_WM_STATE_STICKY"});
        setCardinals(win, "_NET_WM_DESKTOP", {0xFFFFFFFFu});
        applyStrut(w, e);
    }
    xcb_flush(conn());
}

void setMargins(QWidget *w, const QMargins &m)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end())
        return;
    it->spec.margins = m;
    applyGeometry(w);
}

void setDesiredSize(QWidget *w, const QSize &size)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end())
        return;
    it->spec.desiredSize = size;
    applyGeometry(w);
}

void setKeyboard(QWidget *w, Keyboard k)
{
    auto it = g_entries.find(w);
    if (it == g_entries.end() || it->spec.keyboard == k)
        return;
    const Keyboard old = it->spec.keyboard;
    it->spec.keyboard = k;
    if (!w->isVisible())
        return;
    if (old == Keyboard::Exclusive && it->grabbed) {
        xcb_ungrab_keyboard(conn(), XCB_CURRENT_TIME);
        it->grabbed = false;
        xcb_flush(conn());
    } else if (k == Keyboard::Exclusive) {
        it->grabTries = 0;
        tryGrab(w);
    }
}

QPoint screenPos(const QWidget *w, const QPoint &rel)
{
    if (!w)
        return rel;
    const QWidget *top = w->window();
    const QPoint inTop = w->mapTo(top, rel);
    const auto it = g_entries.constFind(top);
    if (it != g_entries.constEnd())
        return inTop + it->origin;
    /* Not one of ours: fall back to X's own idea of where it is. */
    QPoint global = top->mapToGlobal(inTop);
    if (const QScreen *s = top->screen())
        global -= s->geometry().topLeft();
    return global;
}

uint windowPid(quintptr window)
{
    xcb_connection_t *c = conn();
    if (!c || !window)
        return 0;
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        c,
        xcb_get_property(c, 0, xcb_window_t(window), atom("_NET_WM_PID"),
                         XCB_ATOM_CARDINAL, 0, 1),
        nullptr);
    uint pid = 0;
    if (reply && reply->format == 32 && xcb_get_property_value_length(reply) >= 4)
        pid = *static_cast<const quint32 *>(xcb_get_property_value(reply));
    free(reply);
    return pid;
}

} /* namespace Surface */
