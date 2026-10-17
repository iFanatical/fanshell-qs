#include "qtileipc.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcessEnvironment>

#include <sys/socket.h>

namespace {

/* Generous relative to a local round trip, small enough that a wedged qtile
 * can't freeze the bar for a visible beat. */
constexpr int requestTimeoutMs = 500;

} /* namespace */

namespace QtileIpc {

/* libqtile.ipc.find_sockfile() for the X11 backend. */
QString socketPath()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString cache = env.value(QStringLiteral("XDG_CACHE_HOME"));
    if (cache.isEmpty())
        cache = QDir::homePath() + QStringLiteral("/.cache");
    const QString display =
        env.value(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
    return cache + QStringLiteral("/qtile/qtilesocket.") + display;
}

bool available()
{
    return QFileInfo::exists(socketPath());
}

QJsonValue call(const QJsonArray &selectors, const QString &command,
                const QJsonArray &args, bool *ok)
{
    if (ok)
        *ok = false;
    QLocalSocket sock;
    sock.connectToServer(socketPath());
    if (!sock.waitForConnected(requestTimeoutMs))
        return QJsonValue();

    /* lifted = true, as `qtile cmd-obj` sends: qtile then converts the args
     * by the command's annotations into the tuple it calls with (unlifted,
     * a JSON list breaks qtile's call path). Typed JSON values convert to
     * themselves. */
    const QJsonArray request{selectors, command, args, QJsonObject(), true};
    sock.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    if (!sock.waitForBytesWritten(requestTimeoutMs))
        return QJsonValue();
    /* qtile reads until EOF before it answers. QLocalSocket cannot half-close,
     * but its descriptor is an ordinary unix socket. */
    ::shutdown(int(sock.socketDescriptor()), SHUT_WR);

    QByteArray raw;
    while (sock.state() == QLocalSocket::ConnectedState) {
        if (!sock.waitForReadyRead(requestTimeoutMs))
            break;
        raw += sock.readAll();
    }
    raw += sock.readAll();

    const QJsonArray reply = QJsonDocument::fromJson(raw).array();
    if (reply.size() != 2)
        return QJsonValue();
    if (ok)
        *ok = reply.at(0).toInt(-1) == 0;
    return reply.at(1);
}

QString eval(const QString &code, bool *ok)
{
    return call(QJsonArray(), QStringLiteral("eval"), QJsonArray{code}, ok)
        .toString();
}

} /* namespace QtileIpc */
