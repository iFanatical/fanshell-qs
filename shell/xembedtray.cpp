#include <QImage>
#include <QPixmap>
#include <QSocketNotifier>

#include <utility>

#include "tray.h"
#include "xembedtray.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>


namespace {

constexpr long systemTrayRequestDock = 0;
constexpr long xembedEmbeddedNotify = 0;
constexpr int legacyIconSize = 24;

/* X11 errors are asynchronous. A legacy client can destroy its icon between
 * our local validity check and the corresponding server request; that is a
 * normal teardown race, not a reason to terminate the shell. */
int ignoreXError(Display *, XErrorEvent *)
{
    return 0;
}

int channel(unsigned long pixel, unsigned long mask)
{
    if (!mask)
        return 0;
    int shift = 0;
    while (!(mask & 1UL)) {
        mask >>= 1;
        ++shift;
    }
    const unsigned long value = (pixel >> shift) & mask;
    return int((value * 255UL + mask / 2) / mask);
}

QString windowTitle(Display *display, Window window, Atom netWmName,
                    Atom utf8)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(display, window, netWmName, 0, 1024, False,
                           utf8, &actualType, &actualFormat, &count,
                           &remaining, &data) == Success && data) {
        const QString title = QString::fromUtf8(
            reinterpret_cast<const char *>(data), int(count));
        XFree(data);
        if (!title.isEmpty())
            return title;
    }
    char *name = nullptr;
    if (XFetchName(display, window, &name) && name) {
        const QString title = QString::fromLocal8Bit(name);
        XFree(name);
        return title;
    }
    return QStringLiteral("Legacy tray icon");
}

class XEmbedItem final : public SniItem {
public:
    XEmbedItem(XEmbedTray *bridge, Window window)
        : SniItem(), m_bridge(bridge), m_window(window)
    {
        id = QStringLiteral("xembed-%1").arg(qulonglong(window));
        status = QStringLiteral("Active");
    }

    void activate(int, int) override { m_bridge->sendButton(m_window, 1); }
    void secondaryActivate(int, int) override
    {
        m_bridge->sendButton(m_window, 2);
    }
    void contextActivate(int, int) override
    {
        m_bridge->sendButton(m_window, 3);
    }
    void publish(const QImage &image, const QString &name)
    {
        title = name;
        tooltipTitle = name;
        icon = QIcon(QPixmap::fromImage(image));
        emit changed();
    }

private:
    XEmbedTray *m_bridge;
    Window m_window;
};

} // namespace

XEmbedTray *XEmbedTray::instance()
{
    static XEmbedTray *tray = new XEmbedTray;
    return tray;
}

XEmbedTray::XEmbedTray(QObject *parent) : QObject(parent)
{
    if (qEnvironmentVariableIsEmpty("DISPLAY"))
        return;
    m_display = XOpenDisplay(nullptr);
    if (!m_display)
        return;
    XSetErrorHandler(ignoreXError);

    const int screen = DefaultScreen(m_display);
    m_root = RootWindow(m_display, screen);
    m_selection = atom("_NET_SYSTEM_TRAY_S0");
    m_managerAtom = atom("MANAGER");
    m_opcodeAtom = atom("_NET_SYSTEM_TRAY_OPCODE");
    m_xembedAtom = atom("_XEMBED");
    m_netWmNameAtom = atom("_NET_WM_NAME");
    m_utf8Atom = atom("UTF8_STRING");

    int damageError = 0;
    int compositeEvent = 0, compositeError = 0;
    if (!XDamageQueryExtension(m_display, &m_damageEvent, &damageError)
        || !XCompositeQueryExtension(m_display, &compositeEvent,
                                     &compositeError)
        || XGetSelectionOwner(m_display, m_selection) != None) {
        XCloseDisplay(m_display);
        m_display = nullptr;
        return;
    }

    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.event_mask = StructureNotifyMask | SubstructureNotifyMask;
    m_manager = XCreateWindow(
        m_display, m_root, -10000, -10000, legacyIconSize, legacyIconSize,
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask, &attrs);
    /* Hyprland pulls this window onto a monitor, however far off-screen it
     * is, so fanhypr-qs's window rules send it away by this name. It has to
     * stay mapped: the docked icons inside it are read from it. */
    char className[] = "fanshell-qs-xembed";
    XClassHint classHint{className, className};
    XSetClassHint(m_display, m_manager, &classHint);
    const unsigned long orientation = 0; /* horizontal */
    const unsigned long visual = XVisualIDFromVisual(
        DefaultVisual(m_display, screen));
    XChangeProperty(m_display, m_manager,
                    atom("_NET_SYSTEM_TRAY_ORIENTATION"), XA_CARDINAL, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&orientation), 1);
    XChangeProperty(m_display, m_manager, atom("_NET_SYSTEM_TRAY_VISUAL"),
                    XA_VISUALID, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&visual), 1);
    XMapWindow(m_display, m_manager);
    XSetSelectionOwner(m_display, m_selection, m_manager, CurrentTime);
    if (XGetSelectionOwner(m_display, m_selection) != m_manager) {
        XDestroyWindow(m_display, m_manager);
        XCloseDisplay(m_display);
        m_display = nullptr;
        return;
    }
    announceManager();
    XFlush(m_display);

    m_notifier = new QSocketNotifier(ConnectionNumber(m_display),
                                     QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            [this]() { processEvents(); });
}

XEmbedTray::~XEmbedTray()
{
    if (!m_display)
        return;
    if (XGetSelectionOwner(m_display, m_selection) == m_manager)
        XSetSelectionOwner(m_display, m_selection, None, CurrentTime);
    for (const Entry &entry : std::as_const(m_entries))
        if (entry.damage)
            XDamageDestroy(m_display, entry.damage);
    if (m_manager)
        XDestroyWindow(m_display, m_manager);
    XCloseDisplay(m_display);
}

Atom XEmbedTray::atom(const char *name)
{
    return XInternAtom(m_display, name, False);
}

void XEmbedTray::announceManager()
{
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = m_root;
    event.xclient.message_type = m_managerAtom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = CurrentTime;
    event.xclient.data.l[1] = long(m_selection);
    event.xclient.data.l[2] = long(m_manager);
    XSendEvent(m_display, m_root, False, StructureNotifyMask, &event);
}

void XEmbedTray::processEvents()
{
    while (m_display && XPending(m_display)) {
        XEvent event{};
        XNextEvent(m_display, &event);
        if (event.type == ClientMessage
            && event.xclient.message_type == m_opcodeAtom
            && event.xclient.data.l[1] == systemTrayRequestDock) {
            dock(Window(event.xclient.data.l[2]));
        } else if (event.type == DestroyNotify) {
            remove(event.xdestroywindow.window, true);
        } else if (event.type == PropertyNotify) {
            refresh(event.xproperty.window);
        } else if (event.type == Expose) {
            refresh(event.xexpose.window);
        } else if (event.type == m_damageEvent + XDamageNotify) {
            const auto *damage = reinterpret_cast<XDamageNotifyEvent *>(&event);
            const Window window = Window(damage->drawable);
            const auto entry = m_entries.constFind(window);
            if (entry != m_entries.constEnd()
                && entry->damage == damage->damage) {
                XDamageSubtract(m_display, damage->damage, None, None);
                refresh(window);
            }
        }
    }
}

void XEmbedTray::dock(Window window)
{
    if (!window || m_entries.contains(window))
        return;
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(m_display, window, &attributes))
        return;

    XSelectInput(m_display, window,
                 StructureNotifyMask | PropertyChangeMask | ExposureMask);
    XCompositeRedirectWindow(m_display, window, CompositeRedirectAutomatic);
    XReparentWindow(m_display, window, m_manager, 0, 0);
    XResizeWindow(m_display, window, legacyIconSize, legacyIconSize);
    XMapRaised(m_display, window);

    XEvent embedded{};
    embedded.xclient.type = ClientMessage;
    embedded.xclient.window = window;
    embedded.xclient.message_type = m_xembedAtom;
    embedded.xclient.format = 32;
    embedded.xclient.data.l[0] = CurrentTime;
    embedded.xclient.data.l[1] = xembedEmbeddedNotify;
    embedded.xclient.data.l[2] = 0; /* supported XEmbed version */
    embedded.xclient.data.l[3] = long(m_manager);
    XSendEvent(m_display, window, False, NoEventMask, &embedded);

    Entry entry;
    entry.item = new XEmbedItem(this, window);
    entry.damage = XDamageCreate(m_display, window, XDamageReportNonEmpty);
    m_entries.insert(window, entry);
    TrayService::instance()->addInternalItem(entry.item);
    XFlush(m_display);
    refresh(window);
}

void XEmbedTray::remove(Window window, bool windowDestroyed)
{
    auto it = m_entries.find(window);
    if (it == m_entries.end())
        return;
    const Entry entry = it.value();
    m_entries.erase(it);
    /* Destroying a drawable destroys its Damage object on the server too. */
    if (entry.damage && !windowDestroyed)
        XDamageDestroy(m_display, entry.damage);
    TrayService::instance()->removeInternalItem(entry.item);
}

void XEmbedTray::refresh(Window window)
{
    auto it = m_entries.find(window);
    if (it == m_entries.end())
        return;
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(m_display, window, &attributes)
        || attributes.width <= 0 || attributes.height <= 0)
        return;
    XImage *source = XGetImage(m_display, window, 0, 0,
                               unsigned(attributes.width),
                               unsigned(attributes.height), AllPlanes,
                               ZPixmap);
    if (!source)
        return;
    QImage image(source->width, source->height, QImage::Format_ARGB32);
    const unsigned long alphaMask = attributes.depth == 32
        ? 0xffffffffUL
              & ~(source->red_mask | source->green_mask | source->blue_mask)
        : 0;
    bool hasVisibleAlpha = false;
    for (int y = 0; y < source->height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < source->width; ++x) {
            const unsigned long pixel = XGetPixel(source, x, y);
            const int alpha = alphaMask ? channel(pixel, alphaMask) : 255;
            hasVisibleAlpha = hasVisibleAlpha || alpha > 0;
            line[x] = qRgba(channel(pixel, source->red_mask),
                            channel(pixel, source->green_mask),
                            channel(pixel, source->blue_mask), alpha);
        }
    }
    XDestroyImage(source);
    /* Some legacy clients use a 32-bit visual but paint XRGB pixels with an
     * all-zero alpha channel. Treat that as opaque rather than disappearing. */
    if (alphaMask && !hasVisibleAlpha) {
        for (int y = 0; y < image.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                line[x] = qRgb(qRed(line[x]), qGreen(line[x]), qBlue(line[x]));
        }
    }
    static_cast<XEmbedItem *>(it->item)->publish(
        image, windowTitle(m_display, window, m_netWmNameAtom, m_utf8Atom));
}

void XEmbedTray::sendButton(Window window, int button)
{
    if (!m_display || !m_entries.contains(window))
        return;
    /* Wine hit-tests the root coordinates against the icon, and drops the
     * click when they are not over it. */
    int rootX = 0, rootY = 0;
    Window child = None;
    XTranslateCoordinates(m_display, window, m_root, legacyIconSize / 2,
                          legacyIconSize / 2, &rootX, &rootY, &child);
    for (int type : {ButtonPress, ButtonRelease}) {
        XEvent event{};
        event.xbutton.type = type;
        event.xbutton.display = m_display;
        event.xbutton.window = window;
        event.xbutton.root = m_root;
        event.xbutton.time = CurrentTime;
        event.xbutton.x = legacyIconSize / 2;
        event.xbutton.y = legacyIconSize / 2;
        event.xbutton.x_root = rootX;
        event.xbutton.y_root = rootY;
        event.xbutton.state = type == ButtonRelease ? Button1Mask << (button - 1) : 0;
        event.xbutton.button = unsigned(button);
        event.xbutton.same_screen = True;
        XSendEvent(m_display, window, False,
                   type == ButtonPress ? ButtonPressMask : ButtonReleaseMask,
                   &event);
    }
    XFlush(m_display);
}
