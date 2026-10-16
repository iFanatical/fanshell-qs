#include "media.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QTimer>

#include "osd.h"

namespace {

const QString mprisPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString objectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString playerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

QVariant unbox(QVariant value)
{
    while (value.canConvert<QDBusVariant>()) {
        const QVariant inner = qvariant_cast<QDBusVariant>(value).variant();
        if (!inner.isValid() || inner == value)
            break;
        value = inner;
    }
    return value;
}

QString metadataString(const QVariantMap &metadata, const QString &key)
{
    return unbox(metadata.value(key)).toString().trimmed();
}

QString metadataArtist(const QVariantMap &metadata)
{
    const QVariant value = unbox(metadata.value(QStringLiteral("xesam:artist")));
    QStringList artists = value.toStringList();
    if (artists.isEmpty() && value.metaType().id() == QMetaType::QString)
        artists << value.toString();
    return artists.join(QStringLiteral(", ")).trimmed();
}

QVariantMap variantMap(QVariant value)
{
    value = unbox(value);
    if (value.canConvert<QVariantMap>())
        return value.toMap();
    if (value.metaType().id() != qMetaTypeId<QDBusArgument>())
        return {};
    QVariantMap result;
    const QDBusArgument argument = qvariant_cast<QDBusArgument>(value);
    argument >> result;
    return result;
}

} // namespace

MediaControls *MediaControls::instance()
{
    static MediaControls *controls = new MediaControls;
    return controls;
}

MediaControls::MediaControls(QObject *parent) : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.interface())
        connect(bus.interface(), &QDBusConnectionInterface::serviceOwnerChanged,
                this, &MediaControls::serviceOwnerChanged);
    bus.connect(QString(), objectPath, propertiesInterface,
                QStringLiteral("PropertiesChanged"), this,
                SLOT(propertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
    discover();
}

bool MediaControls::isPlayerService(const QString &service)
{
    return service.startsWith(mprisPrefix);
}

void MediaControls::discover()
{
    QDBusConnectionInterface *interface = QDBusConnection::sessionBus().interface();
    if (!interface)
        return;
    const QDBusReply<QStringList> reply = interface->registeredServiceNames();
    if (!reply.isValid())
        return;
    for (const QString &service : reply.value())
        if (isPlayerService(service))
            refresh(service);
}

void MediaControls::serviceOwnerChanged(const QString &name,
                                        const QString &oldOwner,
                                        const QString &newOwner)
{
    if (!isPlayerService(name))
        return;
    if (newOwner.isEmpty()) {
        m_players.remove(name);
        m_ownerServices.remove(oldOwner);
        if (m_pending.service == name)
            m_pending.service.clear();
    } else {
        m_ownerServices.insert(newOwner, name);
        refresh(name);
    }
}

void MediaControls::refresh(const QString &service)
{
    /* Everything here is asynchronous. QDBusInterface would introspect the
     * player synchronously on construction, and a wedged browser can hold
     * such a call for the full 25 s D-Bus timeout -- freezing the bar. */
    QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage ownerMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    ownerMsg << service;
    auto *ownerWatcher =
        new QDBusPendingCallWatcher(bus.asyncCall(ownerMsg), this);
    connect(ownerWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, service, ownerWatcher]() {
                QDBusPendingReply<QString> owner = *ownerWatcher;
                ownerWatcher->deleteLater();
                if (owner.isValid())
                    m_ownerServices.insert(owner.value(), service);
            });

    QDBusMessage msg = QDBusMessage::createMethodCall(
        service, objectPath, propertiesInterface, QStringLiteral("GetAll"));
    msg << playerInterface;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, service, watcher]() {
                QDBusPendingReply<QVariantMap> reply = *watcher;
                watcher->deleteLater();
                if (reply.isValid())
                    apply(service, reply.value());
            });
}

void MediaControls::propertiesChanged(const QString &interface,
                                      const QVariantMap &changed,
                                      const QStringList &invalidated,
                                      const QDBusMessage &message)
{
    if (interface != playerInterface)
        return;
    const QString service = isPlayerService(message.service())
                                ? message.service()
                                : m_ownerServices.value(message.service());
    if (service.isEmpty())
        return;
    apply(service, changed);
    if (!invalidated.isEmpty())
        refresh(service);
}

void MediaControls::apply(const QString &service,
                          const QVariantMap &properties)
{
    Player &player = m_players[service];
    if (properties.contains(QStringLiteral("PlaybackStatus")))
        player.status = unbox(properties.value(
            QStringLiteral("PlaybackStatus"))).toString();
    if (properties.contains(QStringLiteral("CanPlay"))
        || properties.contains(QStringLiteral("CanPause"))) {
        const bool canPlay = unbox(properties.value(
            QStringLiteral("CanPlay"), true)).toBool();
        const bool canPause = unbox(properties.value(
            QStringLiteral("CanPause"), true)).toBool();
        player.canPlayPause = canPlay || canPause;
    }
    if (properties.contains(QStringLiteral("CanGoPrevious")))
        player.canPrevious = unbox(properties.value(
            QStringLiteral("CanGoPrevious"))).toBool();
    if (properties.contains(QStringLiteral("CanGoNext")))
        player.canNext = unbox(properties.value(
            QStringLiteral("CanGoNext"))).toBool();
    if (properties.contains(QStringLiteral("Metadata"))) {
        const QVariantMap metadata = variantMap(
            properties.value(QStringLiteral("Metadata")));
        player.title = metadataString(metadata, QStringLiteral("xesam:title"));
        player.artist = metadataArtist(metadata);
    }
    player.touched = ++m_touchCounter;
    finishTrackChange(service);
}

QString MediaControls::selectPlayer(Action action) const
{
    QString selected;
    int selectedRank = -1;
    quint64 selectedTouch = 0;
    for (auto it = m_players.constBegin(); it != m_players.constEnd(); ++it) {
        const Player &player = it.value();
        /* Transport navigation must never jump into an unrelated paused
         * browser tab. PlayPause keeps the paused fallback so a player that
         * this key just paused can still be resumed. */
        if (action != Action::PlayPause
            && player.status != QLatin1String("Playing"))
            continue;
        const bool capable = action == Action::PlayPause
                                 ? player.canPlayPause
                                 : action == Action::Previous
                                       ? player.canPrevious : player.canNext;
        if (!capable)
            continue;
        const int rank = player.status == QLatin1String("Playing") ? 2
                         : player.status == QLatin1String("Paused") ? 1 : 0;
        if (rank > selectedRank
            || (rank == selectedRank && player.touched > selectedTouch)) {
            selected = it.key();
            selectedRank = rank;
            selectedTouch = player.touched;
        }
    }
    return selected;
}

QString MediaControls::actionMethod(Action action)
{
    if (action == Action::Previous)
        return QStringLiteral("Previous");
    if (action == Action::Next)
        return QStringLiteral("Next");
    return QStringLiteral("PlayPause");
}

void MediaControls::playPause() { invoke(Action::PlayPause); }
void MediaControls::previous() { invoke(Action::Previous); }
void MediaControls::next() { invoke(Action::Next); }

void MediaControls::invoke(Action action)
{
    const QString service = selectPlayer(action);
    if (service.isEmpty()) {
        HardwareOsd::instance()->showMediaUnavailable();
        discover();
        return;
    }

    const Player before = m_players.value(service);
    if (action == Action::PlayPause) {
        showCurrent(action, before);
    } else {
        m_pending.service = service;
        m_pending.oldTitle = before.title;
        m_pending.action = action;
        m_pending.serial = ++m_transitionSerial;
    }

    QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(
        service, objectPath, playerInterface, actionMethod(action)));

    if (action != Action::PlayPause)
        pollTrackChange(service, m_pending.serial, 0);
}

void MediaControls::showCurrent(Action action, const Player &player)
{
    const QString actionName = action == Action::PlayPause
                                   ? (player.status == QLatin1String("Playing")
                                          ? QStringLiteral("Pause")
                                          : QStringLiteral("Play"))
                                   : action == Action::Previous
                                         ? QStringLiteral("Previous")
                                         : QStringLiteral("Next");
    HardwareOsd::instance()->showMedia(actionName, player.title,
                                       player.artist);
}

void MediaControls::finishTrackChange(const QString &service)
{
    if (m_pending.service != service)
        return;
    const Player &player = m_players[service];
    if (player.status != QLatin1String("Playing") || player.title.isEmpty()
        || player.title == m_pending.oldTitle)
        return;

    /* Chromium exposes all of its tabs through one MPRIS service. While a
     * browser audio element swaps sources, that service can briefly publish
     * metadata from an unrelated paused tab before returning to the playing
     * tab. Do not accept the first different title: require the complete
     * title/artist pair to remain stable in Playing state. */
    const QString candidateTitle = player.title;
    const QString candidateArtist = player.artist;
    const quint64 serial = m_pending.serial;
    QTimer::singleShot(400, this,
                       [this, service, serial, candidateTitle,
                        candidateArtist]() {
        if (m_pending.service != service || m_pending.serial != serial)
            return;
        const Player current = m_players.value(service);
        if (current.status != QLatin1String("Playing")
            || current.title != candidateTitle
            || current.artist != candidateArtist)
            return;
        showCurrent(m_pending.action, current);
        m_pending.service.clear();
    });
}

void MediaControls::pollTrackChange(const QString &service, quint64 serial,
                                    int attempt)
{
    /* Players vary considerably here: some publish Metadata with the method
     * reply, while browser players may take more than a second. Re-read the
     * authoritative property for a short bounded period instead of painting
     * a stale cached value on a fixed fallback deadline. */
    static constexpr int delays[] = {120, 180, 250, 350, 500, 700};
    if (attempt >= int(sizeof(delays) / sizeof(delays[0]))) {
        QTimer::singleShot(150, this, [this, service, serial]() {
            if (m_pending.service == service && m_pending.serial == serial) {
                const Player current = m_players.value(service);
                if (current.status == QLatin1String("Playing"))
                    showCurrent(m_pending.action, current);
                else
                    HardwareOsd::instance()->showMediaUnavailable();
                m_pending.service.clear();
            }
        });
        return;
    }
    QTimer::singleShot(delays[attempt], this,
                       [this, service, serial, attempt]() {
        if (m_pending.service != service || m_pending.serial != serial)
            return;
        refresh(service);
        pollTrackChange(service, serial, attempt + 1);
    });
}
