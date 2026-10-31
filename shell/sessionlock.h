/* The lock screen's Wayland side: ext-session-lock-v1 (lockscreen.h has the
 * rest).
 *
 * Under the protocol the compositor itself keeps the session locked -- no
 * grabs, nothing can be mapped or raised over it -- until the client that
 * locked it says unlock. If that client dies first, the session stays
 * locked: Hyprland shows its "lock screen died" screen, and lets a new
 * locker take over when misc:allow_session_lock_restore is set, which the
 * lock screen's supervisor sets before it locks.
 *
 * Qt cannot give a window the lock-surface role, so this is plain
 * wayland-client on the shell's own connection (wlclient.h): one surface
 * per output, painted by `paint` with QPainter into shared-memory buffers,
 * at most once per frame the compositor asks for. Input arrives here as
 * well and is passed on as Qt events: keys through xkbcommon, with the
 * seat's repeat rate, clicks and motion with the surface's output. */
#ifndef FANSHELL_QS_SESSIONLOCK_H
#define FANSHELL_QS_SESSIONLOCK_H

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

class QKeyEvent;
class QPainter;
struct ext_session_lock_surface_v1;
struct ext_session_lock_v1;
struct wl_buffer;
struct wl_callback;
struct wl_cursor_theme;
struct wl_keyboard;
struct wl_pointer;
struct wl_surface;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace Wl {
struct Output;
}

class SessionLock : public QObject {
    Q_OBJECT
public:
    explicit SessionLock(QObject *parent = nullptr);
    ~SessionLock() override;

    /* A compositor that offers the protocol, and a seat. */
    static bool supported();

    /* Lock, and wait up to `timeoutMs` for the compositor to say the
     * session is locked; `paint` must be set first, since it is asked for
     * the first frames meanwhile. False when it refused. */
    bool lock(int timeoutMs);
    void unlock();
    bool locked() const { return m_locked; }

    /* Outputs with a configured surface, and each one's size (logical). */
    QStringList outputs() const;
    QSize size(const QString &output) const;
    /* Repaint one output's surface, or every one's when empty. */
    void damage(const QString &output = QString());
    void setCursorVisible(bool visible);
    bool capsLock() const;

    std::function<void(const QString &output, QPainter &p)> paint;

    /* Callbacks from C. */
    struct Surface;
    static void lockedEvent(void *data, ext_session_lock_v1 *);
    static void finishedEvent(void *data, ext_session_lock_v1 *);

signals:
    /* A surface got its size, or one went away with its output. */
    void surfacesChanged();
    void keyPressed(QKeyEvent *e);
    void pressed(const QString &output, const QPointF &pos);
    void moved();
    /* The compositor ended the lock (another locker took over). */
    void finished();

public:
    /* Used by the listeners. */
    void configure(Surface *s, uint32_t serial, uint32_t w, uint32_t h);
    void frameDone(Surface *s);
    void keymap(int fd, uint32_t size);
    void key(uint32_t key, bool down);
    void modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    void repeatInfo(int32_t rate, int32_t delay);
    void stopRepeat() { m_repeat.stop(); }
    void pointerEnter(wl_surface *surface, uint32_t serial, double x, double y);
    void pointerMotion(double x, double y);
    void pointerButton(uint32_t button, bool down);

private:
    void addSurface(Wl::Output *output);
    void removeSurface(Wl::Output *output);
    void draw(Surface *s);
    void seatChanged();
    void sendKey(uint32_t key, bool repeat);
    void applyCursor();

    ext_session_lock_v1 *m_lock = nullptr;
    bool m_locked = false;
    bool m_finished = false;
    QHash<Wl::Output *, Surface *> m_surfaces;

    wl_keyboard *m_keyboard = nullptr;
    wl_pointer *m_pointer = nullptr;
    xkb_context *m_xkb = nullptr;
    xkb_keymap *m_keymap = nullptr;
    xkb_state *m_state = nullptr;
    QTimer m_repeat;
    uint32_t m_repeatKey = 0;
    int m_repeatRate = 25;
    int m_repeatDelay = 600;

    Surface *m_pointerOn = nullptr;
    QPointF m_pointerPos;
    uint32_t m_enterSerial = 0;
    bool m_cursorVisible = true;
    wl_cursor_theme *m_cursorTheme = nullptr;
    wl_surface *m_cursorSurface = nullptr;
};

#endif
