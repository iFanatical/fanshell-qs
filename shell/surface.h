/* Shell surfaces on X11 -- the X11 counterpart of fanhypr-qs's wlutil.
 *
 * fanhypr-qs describes every top-level it owns (bar, popups, scrim, toasts,
 * OSD, launcher) as a wlr-layer-shell surface: a layer, the screen edges it
 * is anchored to, margins from those edges, an exclusive zone, and how it
 * takes the keyboard. This keeps that vocabulary so the widgets read the same
 * in both trees, and maps it onto X11:
 *
 *   exclusiveZone > 0   a _NET_WM_WINDOW_TYPE_DOCK window with a matching
 *                       _NET_WM_STRUT_PARTIAL. The window manager maps docks
 *                       without managing them and reserves their height, so
 *                       nothing in its config has to agree with the bar size.
 *   everything else     an override-redirect window placed by us. Layer order
 *                       is enforced by raising on show: a surface shown later
 *                       sits above one shown earlier, which is the order the
 *                       shell already shows them in (scrim, then popup).
 *   Keyboard::OnDemand  takes X input focus on show and hands it back to the
 *                       previous window on hide.
 *   Keyboard::Exclusive additionally grabs the keyboard, as dmenu does, so the
 *                       WM's own key bindings cannot fire underneath it.
 *
 * Positions follow the layer-shell convention of output-local coordinates:
 * margins are measured from the anchored edges of the surface's QScreen, and
 * screenPos() reports output-local positions, so popup placement code is the
 * same as under Wayland. */
#ifndef FANSHELL_QS_SURFACE_H
#define FANSHELL_QS_SURFACE_H

#include <QMargins>
#include <QPoint>
#include <QSize>
#include <QString>

class QScreen;
class QWidget;

namespace Surface {

enum Anchor {
    AnchorTop = 1,
    AnchorBottom = 2,
    AnchorLeft = 4,
    AnchorRight = 8,
};

/* Kept for parity with the layer-shell spec; on X11 stacking comes from show
 * order (see above). */
enum class Layer { Background, Bottom, Top, Overlay };

enum class Keyboard {
    None,      /* never focused -- the bar itself, the scrim, toasts */
    OnDemand,  /* focused on show (when activateOnShow), returned on hide */
    Exclusive, /* focused and keyboard grabbed while shown */
};

/* _NET_WM_WINDOW_TYPE, so a compositor's per-type rules (picom `wintypes`)
 * can tell the shell's windows apart. Auto: Dock for an exclusive zone,
 * PopupMenu otherwise. */
enum class WindowType { Auto, Dock, PopupMenu, Notification, Utility, Dialog };

struct SurfaceSpec {
    Layer layer = Layer::Top;
    int anchors = AnchorTop | AnchorLeft;
    QMargins margins;
    /* A 0 in either axis means "fill the space between the anchored edges"
     * when anchored to both, and "keep the widget's own size" otherwise. */
    QSize desiredSize;
    /* >0 reserves that many pixels along the anchored edge (a dock); 0
     * keeps clear of other surfaces' zones; -1 ignores them, measuring
     * margins from the true screen edge. */
    int exclusiveZone = -1;
    Keyboard keyboard = Keyboard::None;
    bool activateOnShow = true;
    WindowType type = WindowType::Auto;
    /* Also used as the window title when the widget has none, so picom and
     * xprop can match a surface by name. */
    QString scope = QStringLiteral("fanshell-qs");
};

/* True on the xcb platform; everything below is a no-op otherwise. */
bool active();

/* Make `w` a shell surface on `screen`. Call before the widget is first
 * shown; later changes go through the setters below. */
void configure(QWidget *w, QScreen *screen, const SurfaceSpec &spec);

void setMargins(QWidget *w, const QMargins &m);
void setDesiredSize(QWidget *w, const QSize &size);
void setKeyboard(QWidget *w, Keyboard k);

/* Output-local position of `rel` within `w`. */
QPoint screenPos(const QWidget *w, const QPoint &rel = QPoint());

/* _NET_WM_PID of an X window, or 0 when it has none. */
uint windowPid(quintptr window);

} /* namespace Surface */

#endif
