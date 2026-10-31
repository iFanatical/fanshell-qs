#include "wlclient.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <cstring>

#include <wayland-client.h>

#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"

namespace Wl {

namespace {

/* Set as the constructor starts: outputs report in during its roundtrips,
 * before instance() has returned. */
Client *g_client = nullptr;

void outputGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
                    const char *, const char *, int32_t)
{
}

void outputMode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}

void outputDone(void *data, wl_output *)
{
    auto *o = static_cast<Output *>(data);
    const bool first = !o->done;
    o->done = true;
    if (first)
        emit g_client->outputAdded(o);
}

void outputScale(void *data, wl_output *, int32_t factor)
{
    static_cast<Output *>(data)->scale = factor > 0 ? factor : 1;
}

void outputName(void *data, wl_output *, const char *name)
{
    static_cast<Output *>(data)->name = QString::fromUtf8(name);
}

void outputDescription(void *, wl_output *, const char *) {}

const wl_output_listener outputListener = {
    outputGeometry, outputMode, outputDone, outputScale, outputName, outputDescription,
};

void seatName(void *, wl_seat *, const char *) {}
const wl_seat_listener seatListener = {&Client::seatCaps, seatName};

void powerMode(void *, zwlr_output_power_v1 *, uint32_t) {}
void powerFailed(void *, zwlr_output_power_v1 *) {}
const zwlr_output_power_v1_listener powerListener = {powerMode, powerFailed};

const wl_registry_listener registryListener = {
    &Client::global, &Client::globalRemove,
};

} /* namespace */

Client *Client::instance()
{
    static Client *c = new Client();
    return c;
}

Client::Client() : QObject(qApp)
{
    g_client = this;
    m_display = wl_display_connect(nullptr);
    if (!m_display)
        return;
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registryListener, this);
    /* Globals, then the outputs' names. */
    wl_display_roundtrip(m_display);
    wl_display_roundtrip(m_display);

    m_notifier = new QSocketNotifier(wl_display_get_fd(m_display),
                                     QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Client::dispatch);
    /* Requests made from Qt slots go out before the loop sleeps. */
    if (auto *ed = QAbstractEventDispatcher::instance())
        connect(ed, &QAbstractEventDispatcher::aboutToBlock, this, &Client::flush);
}

Client::~Client()
{
    if (!m_display)
        return;
    for (Output *o : std::as_const(m_outputs)) {
        wl_output_destroy(o->output);
        delete o;
    }
    wl_display_disconnect(m_display);
}

void Client::global(void *data, wl_registry *registry, uint32_t name,
                    const char *iface, uint32_t version)
{
    auto *c = static_cast<Client *>(data);
    auto bind = [&](const wl_interface *i, uint32_t max) {
        return wl_registry_bind(registry, name, i, qMin(version, max));
    };
    if (!strcmp(iface, wl_compositor_interface.name)) {
        c->m_compositor = static_cast<wl_compositor *>(bind(&wl_compositor_interface, 4));
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        c->m_shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
    } else if (!strcmp(iface, wl_seat_interface.name) && !c->m_seat) {
        c->m_seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 5));
        wl_seat_add_listener(c->m_seat, &seatListener, c);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        if (version < 4)
            return; /* no name event: useless for matching outputs */
        auto *o = new Output;
        o->global = name;
        o->output = static_cast<wl_output *>(bind(&wl_output_interface, 4));
        wl_output_add_listener(o->output, &outputListener, o);
        c->m_outputs.push_back(o);
    } else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name)) {
        c->m_lock = static_cast<ext_session_lock_manager_v1 *>(
            bind(&ext_session_lock_manager_v1_interface, 1));
    } else if (!strcmp(iface, ext_idle_notifier_v1_interface.name)) {
        c->m_idle = static_cast<ext_idle_notifier_v1 *>(
            bind(&ext_idle_notifier_v1_interface, 2));
    } else if (!strcmp(iface, zwlr_output_power_manager_v1_interface.name)) {
        c->m_power = static_cast<zwlr_output_power_manager_v1 *>(
            bind(&zwlr_output_power_manager_v1_interface, 1));
    }
}

void Client::seatCaps(void *data, wl_seat *, uint32_t caps)
{
    auto *c = static_cast<Client *>(data);
    c->m_seatCaps = caps;
    emit c->seatCapabilitiesChanged();
}

void Client::globalRemove(void *data, wl_registry *, uint32_t name)
{
    auto *c = static_cast<Client *>(data);
    for (int i = 0; i < c->m_outputs.size(); ++i) {
        Output *o = c->m_outputs[i];
        if (o->global != name)
            continue;
        c->m_outputs.removeAt(i);
        if (o->done)
            emit c->outputRemoved(o);
        wl_output_destroy(o->output);
        delete o;
        return;
    }
}

void Client::dispatch()
{
    if (wl_display_dispatch(m_display) < 0) {
        /* The compositor went away: nothing more will come. */
        m_notifier->setEnabled(false);
        qWarning("fanshell-qs: lost the Wayland connection");
    }
}

void Client::flush()
{
    if (m_display) {
        wl_display_dispatch_pending(m_display);
        wl_display_flush(m_display);
    }
}

void Client::roundtrip()
{
    if (m_display)
        wl_display_roundtrip(m_display);
}

bool Client::setPower(bool on)
{
    if (!m_power)
        return false;
    for (Output *o : std::as_const(m_outputs)) {
        zwlr_output_power_v1 *p =
            zwlr_output_power_manager_v1_get_output_power(m_power, o->output);
        zwlr_output_power_v1_add_listener(p, &powerListener, nullptr);
        zwlr_output_power_v1_set_mode(p, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON
                                            : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
        /* The mode stays set after the object goes. */
        zwlr_output_power_v1_destroy(p);
    }
    flush();
    return true;
}

/* -------------------------------------------------------------- IdleWatch */

namespace {
const ext_idle_notification_v1_listener idleListener = {
    &IdleWatch::onIdled, &IdleWatch::onResumed,
};
}

bool IdleWatch::supported()
{
    Client *c = Client::instance();
    return c->ok() && c->idleNotifier() && c->seat();
}

IdleWatch::IdleWatch(int ms, bool ignoreInhibitors, QObject *parent) : QObject(parent)
{
    if (!supported() || ms <= 0)
        return;
    Client *c = Client::instance();
    ext_idle_notifier_v1 *n = c->idleNotifier();
    if (ignoreInhibitors
            && ext_idle_notifier_v1_get_version(n)
                   >= EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION_SINCE_VERSION)
        m_note = ext_idle_notifier_v1_get_input_idle_notification(n, uint32_t(ms),
                                                                  c->seat());
    else
        m_note = ext_idle_notifier_v1_get_idle_notification(n, uint32_t(ms), c->seat());
    ext_idle_notification_v1_add_listener(m_note, &idleListener, this);
    c->flush();
}

IdleWatch::~IdleWatch()
{
    if (m_note) {
        ext_idle_notification_v1_destroy(m_note);
        Client::instance()->flush();
    }
}

void IdleWatch::onIdled(void *data, ext_idle_notification_v1 *)
{
    auto *w = static_cast<IdleWatch *>(data);
    w->m_idle = true;
    emit w->idled();
}

void IdleWatch::onResumed(void *data, ext_idle_notification_v1 *)
{
    auto *w = static_cast<IdleWatch *>(data);
    w->m_idle = false;
    emit w->resumed();
}

} /* namespace Wl */
