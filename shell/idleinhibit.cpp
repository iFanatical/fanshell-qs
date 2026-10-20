#include "idleinhibit.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QGuiApplication>

#include <xcb/screensaver.h>
#include <xcb/xcb.h>

#include <cstdlib>

namespace {

constexpr auto service = "org.freedesktop.ScreenSaver";

xcb_connection_t *xconn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

} /* namespace */

IdleInhibitor *IdleInhibitor::instance()
{
    static IdleInhibitor *s = new IdleInhibitor();
    return s;
}

IdleInhibitor::IdleInhibitor(QObject *parent) : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(bus);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    /* A program gone without UnInhibit: its requests go with it. */
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this](const QString &owner) {
                bool any = false;
                for (auto it = m_inhibitors.begin(); it != m_inhibitors.end();) {
                    if (it->owner == owner) {
                        it = m_inhibitors.erase(it);
                        any = true;
                    } else {
                        ++it;
                    }
                }
                m_watcher->removeWatchedService(owner);
                if (any)
                    update();
            });

    if (!bus.registerService(QLatin1String(service))) {
        qWarning("fanshell-qs: %s is already owned; leaving idle inhibition to it",
                 service);
        return;
    }
    for (const char *path : {"/org/freedesktop/ScreenSaver", "/ScreenSaver"})
        bus.registerObject(QLatin1String(path), this,
                           QDBusConnection::ExportScriptableSlots);
}

uint IdleInhibitor::Inhibit(const QString &application, const QString &reason)
{
    const QString owner = calledFromDBus() ? message().service() : QString();
    const uint cookie = m_next++;
    m_inhibitors.insert(cookie, {owner, application, reason});
    if (!owner.isEmpty())
        m_watcher->addWatchedService(owner);
    update();
    return cookie;
}

void IdleInhibitor::UnInhibit(uint cookie)
{
    if (m_inhibitors.remove(cookie))
        update();
}

uint IdleInhibitor::GetSessionIdleTime()
{
    xcb_connection_t *c = xconn();
    if (!c)
        return 0;
    const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_screensaver_query_info_reply_t *info = xcb_screensaver_query_info_reply(
        c, xcb_screensaver_query_info(c, root), nullptr);
    if (!info)
        return 0;
    const uint seconds = info->ms_since_user_input / 1000;
    free(info);
    return seconds;
}

void IdleInhibitor::SimulateUserActivity()
{
    if (xcb_connection_t *c = xconn()) {
        xcb_force_screen_saver(c, XCB_SCREEN_SAVER_RESET);
        xcb_flush(c);
    }
}

void IdleInhibitor::setShellRequest(uint cookie, const QString &name, bool on)
{
    if (on == holds(cookie))
        return;
    if (on)
        m_inhibitors.insert(cookie, {QString(), name, QString()});
    else
        m_inhibitors.remove(cookie);
    update();
}

QStringList IdleInhibitor::reasons() const
{
    QStringList out;
    for (const Inhibitor &i : m_inhibitors)
        out << (i.reason.isEmpty() ? i.application
                                   : i.application + QStringLiteral(": ") + i.reason);
    out.removeDuplicates();
    return out;
}

/* The X server's own blanking and DPMS follow the requests too: suspended
 * while any is held. Suspension is per client and counted, so it is
 * toggled only on the edges. */
void IdleInhibitor::update()
{
    const bool want = inhibited();
    if (want != m_suspended) {
        if (xcb_connection_t *c = xconn()) {
            xcb_screensaver_suspend(c, want ? 1 : 0);
            xcb_flush(c);
        }
        m_suspended = want;
    }
    emit changed();
}
