/* MPRIS media-key backend. Player discovery and state changes are driven by
 * the session bus; there is no polling process and no player-specific CLI. */
#ifndef FANSHELL_QS_MEDIA_H
#define FANSHELL_QS_MEDIA_H

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class QDBusMessage;

class MediaControls : public QObject {
    Q_OBJECT
public:
    static MediaControls *instance();

    void playPause();
    void previous();
    void next();

    /* The player the play/pause key would act on, for the lock screen's
     * controls; an empty title when none is playing or paused. */
    struct NowPlaying {
        QString title;
        QString artist;
        QString artUrl;
        bool playing = false;
        bool canPlayPause = false;
        bool canPrevious = false;
        bool canNext = false;
    };
    NowPlaying nowPlaying() const;

signals:
    /* A player came, went or changed what it plays. */
    void changed();

private slots:
    void serviceOwnerChanged(const QString &name, const QString &oldOwner,
                             const QString &newOwner);
    void propertiesChanged(const QString &interface,
                           const QVariantMap &changed,
                           const QStringList &invalidated,
                           const QDBusMessage &message);

private:
    enum class Action { PlayPause, Previous, Next };
    struct Player {
        QString status;
        QString title;
        QString artist;
        QString artUrl;
        bool canPlayPause = true;
        bool canPrevious = true;
        bool canNext = true;
        quint64 touched = 0;
    };
    struct PendingTransition {
        QString service;
        QString oldTitle;
        Action action = Action::Next;
        quint64 serial = 0;
    };

    explicit MediaControls(QObject *parent = nullptr);
    void discover();
    void refresh(const QString &service);
    void apply(const QString &service, const QVariantMap &properties);
    void invoke(Action action);
    QString selectPlayer(Action action) const;
    void showCurrent(Action action, const Player &player);
    void finishTrackChange(const QString &service);
    void pollTrackChange(const QString &service, quint64 serial, int attempt);
    static bool isPlayerService(const QString &service);
    static QString actionMethod(Action action);

    QHash<QString, Player> m_players;
    QHash<QString, QString> m_ownerServices;
    PendingTransition m_pending;
    quint64 m_touchCounter = 0;
    quint64 m_transitionSerial = 0;
};

#endif
