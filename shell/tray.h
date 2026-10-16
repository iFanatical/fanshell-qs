/* The system tray area and its icons, backed by a hand-rolled
 * StatusNotifierItem (SNI) implementation over QDBus.
 *
 * TrayService acts as org.kde.StatusNotifierWatcher when the name is free
 * otherwise it registers as a
 * StatusNotifierHost against the existing watcher. */
#ifndef FANSHELL_QS_TRAY_H
#define FANSHELL_QS_TRAY_H

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QIcon>
#include <QObject>
#include <QWidget>

#include "theme.h"

/* One StatusNotifierItem proxy. */
class SniItem : public QObject {
    Q_OBJECT
public:
    SniItem(const QString &service, const QString &path,
            QObject *parent = nullptr);
    explicit SniItem(QObject *parent = nullptr);
    virtual ~SniItem() = default;

    QString service;
    QString path;

    QString id;
    QString title;
    QString status;
    QString tooltipTitle;
    QString menuPath;      /* com.canonical.dbusmenu object path, may be "" */
    QIcon icon;

    bool hasMenu() const
    {
        return !menuPath.isEmpty() && menuPath != QLatin1String("/");
    }

    virtual void activate(int x, int y);
    virtual void secondaryActivate(int x, int y);
    virtual void contextActivate(int x, int y);

signals:
    void changed();

private slots:
    void refetch();

private:
    void applyProperties(const QVariantMap &props);

    QString m_iconThemePath;
};

class TrayService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
public:
    static TrayService *instance();

    QVector<SniItem *> items;

    /* StatusNotifierWatcher D-Bus API (when we own the name) */
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ registeredItems)
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ hostRegistered)
    Q_PROPERTY(int ProtocolVersion READ protocolVersion)

    QStringList registeredItems() const;
    bool hostRegistered() const { return true; }
    int protocolVersion() const { return 0; }
    /* Internal compatibility items share the same widgets without being
     * exported as fake D-Bus services. */
    void addInternalItem(SniItem *item);
    void removeInternalItem(SniItem *item);

public slots:
    void RegisterStatusNotifierItem(const QString &serviceOrPath);
    void RegisterStatusNotifierHost(const QString &service);
    void removeItemsForService(const QString &service);

signals:
    void itemsChanged();

    /* watcher signals */
    void StatusNotifierItemRegistered(const QString &item);
    void StatusNotifierItemUnregistered(const QString &item);
    void StatusNotifierHostRegistered();

private:
    explicit TrayService(QObject *parent = nullptr);
    void setupWatcherOrHost();
    void addItem(const QString &service, const QString &path);

    bool m_isWatcher = false;
    QDBusServiceWatcher m_itemWatcher;   /* item services vanishing */
    QDBusServiceWatcher m_watcherWatcher; /* the external watcher vanishing */
};

class TrayMenu;

/* One tray icon in the bar. */
class TrayIconWidget : public QWidget {
    Q_OBJECT
public:
    TrayIconWidget(SniItem *item, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    void openMenu();

    SniItem *m_item;
    TrayMenu *m_menu = nullptr;
    bool m_hover = false;
    Qt::MouseButton m_pressed = Qt::NoButton;
};

/* Row of tray icons; hidden when there are none or when this panel's screen
 * is not the tray host. */
class TrayArea : public QWidget {
    Q_OBJECT
public:
    explicit TrayArea(QWidget *parent = nullptr);

    void setHostsTray(bool hosts);

private:
    void rebuild();

    class QHBoxLayout *m_layout;
    bool m_hostsTray = true;
};

#endif
