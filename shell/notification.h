/* Freedesktop notification daemon, toast stack and history popup.
 *
 * The service owns org.freedesktop.Notifications on the session bus. Toasts
 * always live on WmState::primaryMonitor() (the tray output), while every
 * panel gets its own NotificationWidget and can open the shared history from
 * that monitor. History is intentionally memory-only. */
#ifndef FANSHELL_QS_NOTIFICATION_H
#define FANSHELL_QS_NOTIFICATION_H

#include <QColor>
#include <QDateTime>
#include <QDBusContext>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "widgets.h"

class QTimer;

struct Notification {
    uint id = 0;
    QString appName;
    QString appIcon;
    QString summary;
    QString body;
    QStringList actions;
    QVariantMap hints;
    uint senderPid = 0;
    int urgency = 1;              /* 0 low, 1 normal, 2 critical */
    int remainingMs = 5000;
    QColor color;                 /* frame/accent */
    QColor foreground = Theme::textStrong;
    QIcon icon;
    QImage image;                 /* notification content image, not app icon */
    QDateTime receivedAt;
    bool visible = false;
};

class NotificationService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    static NotificationService *instance();

    bool start();
    bool dnd() const { return m_dnd; }
    const QVector<Notification> &history() const { return m_history; }
    QVector<uint> visibleIds() const;
    const Notification *find(uint id) const;

    void toggleDnd();
    void setDnd(bool enabled);
    /* While the screen is locked no toast is shown, urgent ones included:
     * they would show over the lock screen. They stay in the history. */
    void setLocked(bool locked);
    void setHovered(uint id);
    void dismiss(uint id, uint reason = 2);
    void clearAll();
    void activate(uint id);
    /* A non-default action button on a visible toast. */
    void invokeAction(uint id, const QString &key);

public slots:
    Q_SCRIPTABLE QStringList GetCapabilities();
    Q_SCRIPTABLE uint Notify(const QString &appName, uint replacesId,
                             const QString &appIcon, const QString &summary,
                             const QString &body, const QStringList &actions,
                             const QVariantMap &hints, int expireTimeout);
    Q_SCRIPTABLE void CloseNotification(uint id);
    Q_SCRIPTABLE void GetServerInformation(QString &name, QString &vendor,
                                           QString &version,
                                           QString &specVersion);

signals:
    void changed();
    Q_SCRIPTABLE void NotificationClosed(uint id, uint reason);
    Q_SCRIPTABLE void ActionInvoked(uint id, const QString &actionKey);

private:
    explicit NotificationService(QObject *parent = nullptr);
    void tick();
    void scheduleExpiry();
    void applyStyle(Notification &n, int requestedTimeout);
    bool focusCertainSender(const Notification &n);

    QVector<Notification> m_history; /* newest first */
    uint m_nextId = 1;
    uint m_hovered = 0;
    bool m_dnd = false;
    bool m_locked = false;
    bool m_started = false;
    qint64 m_lastTick = 0;
    QTimer *m_expiryTimer = nullptr;
};

class NotificationHistoryPopup;

class NotificationWidget : public BarPill {
    Q_OBJECT
public:
    explicit NotificationWidget(QWidget *parent = nullptr);

private:
    void sync();
    NotificationHistoryPopup *m_popup;
};

#endif
