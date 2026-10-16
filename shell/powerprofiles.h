/* Standard net.hadess.PowerProfiles D-Bus state and full-width System tile. */
#ifndef FANSHELL_QS_POWERPROFILES_H
#define FANSHELL_QS_POWERPROFILES_H

#include <QObject>
#include <QStringList>
#include <QWidget>

class QDBusServiceWatcher;
class ShellButton;

class PowerProfileState : public QObject {
    Q_OBJECT
public:
    static PowerProfileState *instance();

    bool available() const { return m_available; }
    QString activeProfile() const { return m_active; }
    QStringList profiles() const { return m_profiles; }
    void setProfile(const QString &profile);

signals:
    void changed();

private slots:
    void propertiesChanged(const QString &interface,
                           const QVariantMap &changed,
                           const QStringList &invalidated);

private:
    explicit PowerProfileState(QObject *parent = nullptr);
    void refresh();
    void applyProperties(const QVariantMap &properties);

    QDBusServiceWatcher *m_watcher;
    bool m_available = false;
    QString m_active;
    QStringList m_profiles;
};

class PowerProfileWidget : public QWidget {
    Q_OBJECT
public:
    explicit PowerProfileWidget(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void sync();

    ShellButton *m_saver;
    ShellButton *m_balanced;
    ShellButton *m_performance;
};

#endif
