/* A Wayland connection of the shell's own, for the protocols Qt does not
 * speak: idle notification (ext-idle-notify-v1), output power
 * (wlr-output-power-management-unstable-v1, what `hyprctl dispatch dpms`
 * does) and the session lock (ext-session-lock-v1, sessionlock.h).
 *
 * It is a second connection beside Qt's rather than Qt's own wl_display, so
 * nothing here depends on QtWayland's internals or its event queue; its fd
 * is read from the Qt event loop, and requests are flushed before it
 * blocks. Outputs are tracked with their names (wl_output v4), which are the
 * names QScreen and Hyprland use. */
#ifndef FANSHELL_QS_WLCLIENT_H
#define FANSHELL_QS_WLCLIENT_H

#include <QObject>
#include <QString>
#include <QVector>

struct wl_compositor;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_seat;
struct wl_shm;
struct ext_idle_notification_v1;
struct ext_idle_notifier_v1;
struct ext_session_lock_manager_v1;
struct zwlr_output_power_manager_v1;
class QSocketNotifier;

namespace Wl {

struct Output {
    uint32_t global = 0;
    wl_output *output = nullptr;
    QString name;
    int scale = 1;
    bool done = false; /* the first `done` arrived: name and scale known */
};

class Client : public QObject {
    Q_OBJECT
public:
    /* The process's connection, made on first use; check ok(). */
    static Client *instance();

    bool ok() const { return m_display != nullptr; }
    wl_display *display() const { return m_display; }
    wl_compositor *compositor() const { return m_compositor; }
    wl_shm *shm() const { return m_shm; }
    wl_seat *seat() const { return m_seat; }
    /* WL_SEAT_CAPABILITY_* the seat has now. */
    uint32_t seatCapabilities() const { return m_seatCaps; }
    ext_session_lock_manager_v1 *lockManager() const { return m_lock; }
    ext_idle_notifier_v1 *idleNotifier() const { return m_idle; }

    /* Outputs announced so far, in announcement order. */
    QVector<Output *> outputs() const { return m_outputs; }

    /* Power every output down, or back up. False where the compositor
     * offers no output power management. */
    bool setPower(bool on);

    void flush();
    /* Block until the compositor has handled everything sent so far. */
    void roundtrip();

signals:
    void outputAdded(Wl::Output *output);
    /* Emitted before the Output is freed. */
    void outputRemoved(Wl::Output *output);
    void seatCapabilitiesChanged();

public:
    /* Registry and seat callbacks. */
    static void seatCaps(void *data, wl_seat *, uint32_t caps);
    static void global(void *data, wl_registry *, uint32_t name, const char *iface,
                       uint32_t version);
    static void globalRemove(void *data, wl_registry *, uint32_t name);

private:
    Client();
    ~Client() override;
    void dispatch();

    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_compositor *m_compositor = nullptr;
    wl_shm *m_shm = nullptr;
    wl_seat *m_seat = nullptr;
    uint32_t m_seatCaps = 0;
    ext_session_lock_manager_v1 *m_lock = nullptr;
    ext_idle_notifier_v1 *m_idle = nullptr;
    zwlr_output_power_manager_v1 *m_power = nullptr;
    QVector<Output *> m_outputs;
    QSocketNotifier *m_notifier = nullptr;
};

/* "Idle for `ms`": idled() after that long without input, resumed() at the
 * next input. As ext-idle-notify-v1 has it, the compositor holds idled()
 * back while a program inhibits idling (a video playing fullscreen, through
 * zwp_idle_inhibit) unless `ignoreInhibitors`. A no-op without the protocol
 * (supported() is false). */
class IdleWatch : public QObject {
    Q_OBJECT
public:
    IdleWatch(int ms, bool ignoreInhibitors, QObject *parent = nullptr);
    ~IdleWatch() override;

    static bool supported();
    bool idle() const { return m_idle; }

    /* Notification callbacks. */
    static void onIdled(void *data, ext_idle_notification_v1 *);
    static void onResumed(void *data, ext_idle_notification_v1 *);

signals:
    void idled();
    void resumed();

private:
    ext_idle_notification_v1 *m_note = nullptr;
    bool m_idle = false;
};

} /* namespace Wl */

#endif
