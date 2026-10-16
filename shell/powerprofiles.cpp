#include "powerprofiles.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QHBoxLayout>
#include <QPainter>

#include "theme.h"
#include "widgets.h"

namespace {
constexpr auto service = "net.hadess.PowerProfiles";
constexpr auto path = "/net/hadess/PowerProfiles";
constexpr auto interface = "net.hadess.PowerProfiles";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";

QVariant unbox(const QVariant &value)
{
    return value.canConvert<QDBusVariant>()
               ? value.value<QDBusVariant>().variant()
               : value;
}

QStringList profileNames(const QVariant &value)
{
    QStringList names;
    const QVariant raw = unbox(value);
    if (raw.canConvert<QDBusArgument>()) {
        QDBusArgument array = raw.value<QDBusArgument>();
        array.beginArray();
        while (!array.atEnd()) {
            QVariantMap entry;
            array >> entry;
            const QString name = unbox(entry.value(QStringLiteral("Profile")))
                                     .toString();
            if (!name.isEmpty() && !names.contains(name))
                names.push_back(name);
        }
        array.endArray();
    } else {
        const QVariantList list = raw.toList();
        for (const QVariant &item : list) {
            const QString name = item.toMap().value(QStringLiteral("Profile"))
                                     .toString();
            if (!name.isEmpty() && !names.contains(name))
                names.push_back(name);
        }
    }
    return names;
}
} /* namespace */

PowerProfileState *PowerProfileState::instance()
{
    static PowerProfileState *state = new PowerProfileState();
    return state;
}

PowerProfileState::PowerProfileState(QObject *parent) : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    m_watcher = new QDBusServiceWatcher(
        QString::fromLatin1(service), bus,
        QDBusServiceWatcher::WatchForRegistration
            | QDBusServiceWatcher::WatchForUnregistration,
        this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this,
            [this]() { refresh(); });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this]() {
                if (!m_available)
                    return;
                m_available = false;
                m_active.clear();
                m_profiles.clear();
                emit changed();
            });
    bus.connect(QString::fromLatin1(service), QString::fromLatin1(path),
                QString::fromLatin1(propertiesInterface),
                QStringLiteral("PropertiesChanged"), this,
                SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    refresh();
}

void PowerProfileState::refresh()
{
    /* Asynchronous: at login the provider may still be D-Bus-activating, and
     * a blocking call would hold up the whole shell until it answers. */
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(propertiesInterface), QStringLiteral("GetAll"));
    msg << QString::fromLatin1(interface);
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher]() {
                QDBusPendingReply<QVariantMap> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid()) {
                    if (m_available) {
                        m_available = false;
                        m_active.clear();
                        m_profiles.clear();
                        emit changed();
                    }
                    return;
                }
                applyProperties(reply.value());
            });
}

void PowerProfileState::applyProperties(const QVariantMap &properties)
{
    const bool oldAvailable = m_available;
    const QString oldActive = m_active;
    const QStringList oldProfiles = m_profiles;
    m_available = true;
    if (properties.contains(QStringLiteral("ActiveProfile")))
        m_active = unbox(properties.value(QStringLiteral("ActiveProfile")))
                       .toString();
    if (properties.contains(QStringLiteral("Profiles")))
        m_profiles = profileNames(properties.value(QStringLiteral("Profiles")));
    if (oldAvailable != m_available || oldActive != m_active
            || oldProfiles != m_profiles)
        emit changed();
}

void PowerProfileState::propertiesChanged(const QString &changedInterface,
                                          const QVariantMap &changed,
                                          const QStringList &invalidated)
{
    if (changedInterface != QLatin1String(interface))
        return;
    if (invalidated.contains(QStringLiteral("ActiveProfile"))
            || invalidated.contains(QStringLiteral("Profiles"))) {
        refresh();
        return;
    }
    applyProperties(changed);
}

void PowerProfileState::setProfile(const QString &profile)
{
    if (!m_available || !m_profiles.contains(profile))
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(propertiesInterface), QStringLiteral("Set"));
    msg << QString::fromLatin1(interface) << QStringLiteral("ActiveProfile")
        << QVariant::fromValue(QDBusVariant(profile));
    QDBusConnection::systemBus().asyncCall(msg);
}

PowerProfileWidget::PowerProfileWidget(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(Theme::pillHeight * 2);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(Theme::popupMargin, 0, Theme::popupMargin, 0);
    layout->setSpacing(Theme::rowSpacing);
    auto *icon = new TextItem(this);
    icon->setText(QStringLiteral("\U000F0425")); /* mdi power-settings */
    icon->setPixelSize(Theme::bodyFontSize);
    icon->setColor(Theme::accent);
    m_saver = new ShellButton(QStringLiteral("Saver"), this);
    m_balanced = new ShellButton(QStringLiteral("Balanced"), this);
    m_performance = new ShellButton(QStringLiteral("Performance"), this);
    setToolTip(QStringLiteral("Power profile"));
    layout->addWidget(icon, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    layout->addWidget(m_saver, 0, Qt::AlignVCenter);
    layout->addWidget(m_balanced, 0, Qt::AlignVCenter);
    layout->addWidget(m_performance, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    auto select = [](const QString &profile) {
        PowerProfileState::instance()->setProfile(profile);
    };
    connect(m_saver, &ShellButton::activated, this,
            [select]() { select(QStringLiteral("power-saver")); });
    connect(m_balanced, &ShellButton::activated, this,
            [select]() { select(QStringLiteral("balanced")); });
    connect(m_performance, &ShellButton::activated, this,
            [select]() { select(QStringLiteral("performance")); });
    connect(PowerProfileState::instance(), &PowerProfileState::changed, this,
            &PowerProfileWidget::sync);
    sync();
}

void PowerProfileWidget::sync()
{
    PowerProfileState *state = PowerProfileState::instance();
    const QStringList profiles = state->profiles();
    setVisible(state->available() && !profiles.isEmpty());
    m_saver->setEnabled(profiles.contains(QStringLiteral("power-saver")));
    m_balanced->setEnabled(profiles.contains(QStringLiteral("balanced")));
    m_performance->setEnabled(profiles.contains(QStringLiteral("performance")));
    m_saver->setActive(state->activeProfile() == QLatin1String("power-saver"));
    m_balanced->setActive(state->activeProfile() == QLatin1String("balanced"));
    m_performance->setActive(
        state->activeProfile() == QLatin1String("performance"));
    update();
}

void PowerProfileWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    Theme::paintRect(painter, rect(), Theme::surface, Theme::radius * 2);
}
