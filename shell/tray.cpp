#include "tray.h"

#include <QScreen>

#include "surface.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDirIterator>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QtEndian>

#include "traymenu.h"

static const char *kSniIface = "org.kde.StatusNotifierItem";
static const char *kWatcherService = "org.kde.StatusNotifierWatcher";
static const char *kWatcherPath = "/StatusNotifierWatcher";
static const char *kWatcherIface = "org.kde.StatusNotifierWatcher";

/* Largest SNI pixmap side we will decode; real ones are 16-256px. */
constexpr int maxSniIconSide = 1024;

/* Decode one a(iiay) icon-pixmap list into a QIcon. Data is ARGB32 in
 * network byte order. */
static QIcon iconFromSniPixmaps(const QDBusArgument &arg)
{
    QIcon icon;
    arg.beginArray();
    while (!arg.atEnd()) {
        int w = 0, h = 0;
        QByteArray bytes;
        arg.beginStructure();
        arg >> w >> h >> bytes;
        arg.endStructure();
        /* Sizes come from any client on the session bus: bound them (tray
         * pixmaps are icon-sized) and do the arithmetic in 64 bits, so a
         * bogus header can't overflow past the size check. */
        if (w > 0 && h > 0 && w <= maxSniIconSide && h <= maxSniIconSide
                && qint64(bytes.size()) >= qint64(w) * h * 4) {
            QImage img(w, h, QImage::Format_ARGB32);
            const uchar *src = reinterpret_cast<const uchar *>(bytes.constData());
            for (int y = 0; y < h; y++) {
                QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < w; x++)
                    dst[x] = qFromBigEndian<quint32>(src + (y * w + x) * 4);
            }
            icon.addPixmap(QPixmap::fromImage(img));
        }
    }
    arg.endArray();
    return icon;
}

/* Icon lookup for items that ship their own theme dir (IconThemePath).
 *
 * Finding the files means a recursive directory walk, and items re-announce
 * their icon (NewIcon) whenever they like -- animated ones constantly. So the
 * file paths are cached per (dir, name); a fresh QIcon is still built from
 * them each time, which keeps rewritten-in-place icon files current. Only hits
 * are cached: an app may write a new icon file just before naming it. */
static QIcon iconFromThemePath(const QString &themePath, const QString &name)
{
    static QHash<QString, QStringList> cache;
    const QString key = themePath + QLatin1Char('\n') + name;

    QStringList files = cache.value(key);
    if (files.isEmpty()) {
        QDirIterator it(themePath, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QFileInfo fi = it.fileInfo();
            if (!fi.isFile())
                continue;
            if (fi.completeBaseName() == name
                    && (fi.suffix() == QLatin1String("png")
                        || fi.suffix() == QLatin1String("svg")
                        || fi.suffix() == QLatin1String("xpm")))
                files << f;
        }
        if (!files.isEmpty())
            cache.insert(key, files);
    }

    QIcon icon;
    for (const QString &f : std::as_const(files))
        if (QFileInfo::exists(f))
            icon.addFile(f);
    if (icon.isNull())
        cache.remove(key); /* files went away; walk again next time */
    return icon;
}

/* --------------------------------------------------------------- SniItem -- */

SniItem::SniItem(const QString &service_, const QString &path_,
                 QObject *parent)
    : QObject(parent), service(service_), path(path_)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    for (const char *sig : {"NewIcon", "NewAttentionIcon", "NewOverlayIcon",
                            "NewTitle", "NewToolTip", "NewStatus"})
        bus.connect(service, path, QString::fromLatin1(kSniIface),
                    QString::fromLatin1(sig), this, SLOT(refetch()));
    refetch();
}

SniItem::SniItem(QObject *parent) : QObject(parent)
{
}

void SniItem::refetch()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        service, path, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("GetAll"));
    msg << QString::fromLatin1(kSniIface);
    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<QVariantMap> reply = *w;
                w->deleteLater();
                if (reply.isValid())
                    applyProperties(reply.value());
            });
}

void SniItem::applyProperties(const QVariantMap &props)
{
    id = props.value(QStringLiteral("Id")).toString();
    title = props.value(QStringLiteral("Title")).toString();
    status = props.value(QStringLiteral("Status")).toString();
    m_iconThemePath = props.value(QStringLiteral("IconThemePath")).toString();

    const QVariant menuV = props.value(QStringLiteral("Menu"));
    menuPath.clear();
    if (menuV.canConvert<QDBusObjectPath>())
        menuPath = menuV.value<QDBusObjectPath>().path();
    else if (menuV.metaType() == QMetaType::fromType<QDBusArgument>()) {
        QDBusObjectPath p;
        menuV.value<QDBusArgument>() >> p;
        menuPath = p.path();
    }

    tooltipTitle.clear();
    const QVariant ttV = props.value(QStringLiteral("ToolTip"));
    if (ttV.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument arg = ttV.value<QDBusArgument>();
        /* (s a(iiay) s s): icon-name, icon-data, title, text */
        QString iconName, ttTitle, ttText;
        arg.beginStructure();
        arg >> iconName;
        arg.beginArray();
        while (!arg.atEnd()) {
            int w, h;
            QByteArray b;
            arg.beginStructure();
            arg >> w >> h >> b;
            arg.endStructure();
        }
        arg.endArray();
        arg >> ttTitle >> ttText;
        arg.endStructure();
        tooltipTitle = ttTitle;
    }

    const bool attention = status == QLatin1String("NeedsAttention");
    QString iconName =
        attention ? props.value(QStringLiteral("AttentionIconName")).toString()
                  : QString();
    if (iconName.isEmpty())
        iconName = props.value(QStringLiteral("IconName")).toString();

    icon = QIcon();
    if (!iconName.isEmpty()) {
        if (!m_iconThemePath.isEmpty())
            icon = iconFromThemePath(m_iconThemePath, iconName);
        if (icon.isNull())
            icon = QIcon::fromTheme(iconName);
    }
    if (icon.isNull()) {
        const QVariant pixV =
            attention ? props.value(QStringLiteral("AttentionIconPixmap"))
                      : props.value(QStringLiteral("IconPixmap"));
        const QVariant fallback = props.value(QStringLiteral("IconPixmap"));
        for (const QVariant &v : {pixV, fallback}) {
            if (v.metaType() == QMetaType::fromType<QDBusArgument>())
                icon = iconFromSniPixmaps(v.value<QDBusArgument>());
            if (!icon.isNull())
                break;
        }
    }

    emit changed();
}

void SniItem::activate(int x, int y)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        service, path, QString::fromLatin1(kSniIface),
        QStringLiteral("Activate"));
    msg << x << y;
    QDBusConnection::sessionBus().asyncCall(msg);
}

void SniItem::secondaryActivate(int x, int y)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        service, path, QString::fromLatin1(kSniIface),
        QStringLiteral("SecondaryActivate"));
    msg << x << y;
    QDBusConnection::sessionBus().asyncCall(msg);
}

void SniItem::contextActivate(int, int)
{
}

/* ------------------------------------------------------------ TrayService */

TrayService *TrayService::instance()
{
    static TrayService *s = new TrayService();
    return s;
}

TrayService::TrayService(QObject *parent) : QObject(parent)
{
    m_itemWatcher.setConnection(QDBusConnection::sessionBus());
    m_itemWatcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_itemWatcher, &QDBusServiceWatcher::serviceUnregistered, this,
            &TrayService::removeItemsForService);

    m_watcherWatcher.setConnection(QDBusConnection::sessionBus());
    m_watcherWatcher.addWatchedService(QString::fromLatin1(kWatcherService));
    m_watcherWatcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watcherWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, [this]() {
                if (!m_isWatcher)
                    setupWatcherOrHost();
            });

    setupWatcherOrHost();
}

void TrayService::setupWatcherOrHost()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;

    if (bus.registerService(QString::fromLatin1(kWatcherService))) {
        m_isWatcher = true;
        bus.registerObject(QString::fromLatin1(kWatcherPath), this,
                           QDBusConnection::ExportAllSlots
                               | QDBusConnection::ExportAllSignals
                               | QDBusConnection::ExportAllProperties);
        emit StatusNotifierHostRegistered();
        return;
    }

    /* Someone else is the watcher: register as a host and mirror its items. */
    m_isWatcher = false;
    const QString hostName =
        QStringLiteral("org.freedesktop.StatusNotifierHost-")
        + QString::number(QCoreApplication::applicationPid());
    bus.registerService(hostName);

    QDBusMessage reg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kWatcherService),
        QString::fromLatin1(kWatcherPath),
        QString::fromLatin1(kWatcherIface),
        QStringLiteral("RegisterStatusNotifierHost"));
    reg << hostName;
    bus.asyncCall(reg);

    bus.connect(QString::fromLatin1(kWatcherService),
                QString::fromLatin1(kWatcherPath),
                QString::fromLatin1(kWatcherIface),
                QStringLiteral("StatusNotifierItemRegistered"), this,
                SLOT(RegisterStatusNotifierItem(QString)));
    bus.connect(QString::fromLatin1(kWatcherService),
                QString::fromLatin1(kWatcherPath),
                QString::fromLatin1(kWatcherIface),
                QStringLiteral("StatusNotifierItemUnregistered"), this,
                SLOT(removeItemsForService(QString)));

    /* Initial list */
    QDBusMessage get = QDBusMessage::createMethodCall(
        QString::fromLatin1(kWatcherService),
        QString::fromLatin1(kWatcherPath),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    get << QString::fromLatin1(kWatcherIface)
        << QStringLiteral("RegisteredStatusNotifierItems");
    QDBusPendingCall call = bus.asyncCall(get);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<QVariant> reply = *w;
                w->deleteLater();
                if (!reply.isValid())
                    return;
                const QStringList list = reply.value().toStringList();
                for (const QString &s : list)
                    RegisterStatusNotifierItem(s);
            });
}

QStringList TrayService::registeredItems() const
{
    QStringList out;
    for (SniItem *it : items)
        out << it->service + it->path;
    return out;
}

void TrayService::RegisterStatusNotifierItem(const QString &serviceOrPath)
{
    QString service, path;
    if (serviceOrPath.startsWith(QLatin1Char('/'))) {
        /* Registered by object path: the item lives on the caller. As a
         * mirror of another watcher this form can't arrive (its signals
         * carry service+path strings). */
        const QDBusMessage &m = message();
        service = m.service();
        path = serviceOrPath;
    } else if (serviceOrPath.contains(QLatin1Char('/'))) {
        const int i = serviceOrPath.indexOf(QLatin1Char('/'));
        service = serviceOrPath.left(i);
        path = serviceOrPath.mid(i);
    } else {
        service = serviceOrPath;
        path = QStringLiteral("/StatusNotifierItem");
    }
    if (service.isEmpty())
        return;
    addItem(service, path);
}

void TrayService::RegisterStatusNotifierHost(const QString &service)
{
    Q_UNUSED(service);
    emit StatusNotifierHostRegistered();
}

void TrayService::addItem(const QString &service, const QString &path)
{
    for (SniItem *it : items)
        if (it->service == service && it->path == path)
            return;
    auto *item = new SniItem(service, path, this);
    items.push_back(item);
    m_itemWatcher.addWatchedService(service);
    if (m_isWatcher)
        emit StatusNotifierItemRegistered(service + path);
    emit itemsChanged();
}

void TrayService::removeItemsForService(const QString &service)
{
    /* Accepts both a bare service name (QDBusServiceWatcher) and the
     * "service/path" form (external watcher's Unregistered signal). */
    QString svc = service;
    const int slash = svc.indexOf(QLatin1Char('/'));
    if (slash > 0)
        svc = svc.left(slash);
    bool removed = false;
    for (int i = items.size() - 1; i >= 0; i--) {
        if (items[i]->service == svc) {
            if (m_isWatcher)
                emit StatusNotifierItemUnregistered(items[i]->service
                                                    + items[i]->path);
            items[i]->deleteLater();
            items.removeAt(i);
            removed = true;
        }
    }
    m_itemWatcher.removeWatchedService(svc);
    if (removed)
        emit itemsChanged();
}

void TrayService::addInternalItem(SniItem *item)
{
    if (!item || items.contains(item))
        return;
    item->setParent(this);
    items.push_back(item);
    emit itemsChanged();
}

void TrayService::removeInternalItem(SniItem *item)
{
    if (!items.removeOne(item))
        return;
    item->deleteLater();
    emit itemsChanged();
}

/* --------------------------------------------------------- TrayIconWidget */

TrayIconWidget::TrayIconWidget(SniItem *item, QWidget *parent)
    : QWidget(parent), m_item(item)
{
    setFixedSize(Theme::trayItemSize, Theme::trayItemSize);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    connect(item, &SniItem::changed, this,
            static_cast<void (QWidget::*)()>(&QWidget::update));
}

void TrayIconWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    Theme::paintRect(p, rect(),
                     m_hover ? Theme::surface : Theme::transparent,
                     Theme::smallRadius);
    const QPixmap pm = m_item->icon.pixmap(
        QSize(Theme::trayIconSize, Theme::trayIconSize));
    if (!pm.isNull()) {
        /* PreserveAspectFit inside 18x18, centered */
        QSizeF sz = pm.deviceIndependentSize();
        sz.scale(Theme::trayIconSize, Theme::trayIconSize,
                 Qt::KeepAspectRatio);
        const QRectF target(QPointF((width() - sz.width()) / 2.0,
                                    (height() - sz.height()) / 2.0),
                            sz);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(target, pm, pm.rect());
    } else {
        QString t = m_item->tooltipTitle;
        if (t.isEmpty())
            t = m_item->title;
        if (t.isEmpty())
            t = m_item->id;
        if (t.isEmpty())
            t = QStringLiteral("?");
        p.setFont(Theme::font(Theme::tinyFontSize, true));
        p.setPen(Theme::text);
        p.drawText(rect(), Qt::AlignCenter, t.left(1).toUpper());
    }
}

void TrayIconWidget::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}

void TrayIconWidget::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void TrayIconWidget::mousePressEvent(QMouseEvent *e)
{
    m_pressed = e->button();
}

void TrayIconWidget::openMenu()
{
    if (!m_item->hasMenu())
        return;
    if (!m_menu)
        m_menu = new TrayMenu(m_item->service, m_item->menuPath, this);
    m_menu->togglePopup();
}

void TrayIconWidget::mouseReleaseEvent(QMouseEvent *e)
{
    const bool in = rect().contains(e->pos());
    const Qt::MouseButton b = e->button();
    if (in && b == m_pressed) {
        /* Advisory "where the icon is" hint for the app's own menu, in
         * root-window coordinates as StatusNotifierItem expects on X11. */
        QPoint g = Surface::screenPos(this, QPoint(0, height()));
        if (const QScreen *s = screen())
            g += s->geometry().topLeft();
        if (b == Qt::LeftButton) {
            /* Toggle the themed menu when the app has one; otherwise trigger
             * its primary action. */
            if (m_item->hasMenu())
                openMenu();
            else
                m_item->activate(g.x(), g.y());
        } else if (b == Qt::MiddleButton) {
            m_item->secondaryActivate(g.x(), g.y());
        } else if (b == Qt::RightButton) {
            if (m_item->hasMenu())
                openMenu();
            else
                m_item->contextActivate(g.x(), g.y());
        }
    }
    m_pressed = Qt::NoButton;
}

/* ---------------------------------------------------------------- TrayArea */

TrayArea::TrayArea(QWidget *parent) : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(Theme::compactSpacing);
    connect(TrayService::instance(), &TrayService::itemsChanged, this,
            &TrayArea::rebuild);
    rebuild();
}

void TrayArea::setHostsTray(bool hosts)
{
    if (m_hostsTray == hosts)
        return;
    m_hostsTray = hosts;
    rebuild();
}

void TrayArea::rebuild()
{
    while (QLayoutItem *it = m_layout->takeAt(0)) {
        if (it->widget()) {
            it->widget()->hide(); /* deleteLater leaves it painted a tick */
            it->widget()->deleteLater();
        }
        delete it;
    }
    const QVector<SniItem *> &items = TrayService::instance()->items;
    for (SniItem *item : items) {
        auto *w = new TrayIconWidget(item, this);
        m_layout->addWidget(w, 0, Qt::AlignVCenter);
        w->show();
    }
    setVisible(m_hostsTray && !items.isEmpty());
}
