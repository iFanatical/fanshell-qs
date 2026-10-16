#include "oxwmipc.h"

#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTimer>

#include <unistd.h>

namespace {

/* Reconnect backoff for the event stream (WM restart, or the shell having
 * been started before oxwm finished coming up). */
constexpr int reconnectMs = 1000;
/* Generous relative to a local socket round trip, small enough that a wedged
 * WM can't freeze the bar for a visible beat. */
constexpr int requestTimeoutMs = 500;

/* ":0", ":0.0" and "host:0" all mean display 0; the WM names its socket
 * directory after just the number. */
QString displayNumber()
{
    const QString display = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("DISPLAY"));
    const int colon = display.lastIndexOf(QLatin1Char(':'));
    if (colon < 0)
        return QStringLiteral("0");
    QString number = display.mid(colon + 1);
    const int dot = number.indexOf(QLatin1Char('.'));
    if (dot >= 0)
        number.truncate(dot);
    return number.isEmpty() ? QStringLiteral("0") : number;
}

} /* namespace */

namespace OxwmIpc {

QString socketDir()
{
    const QString runtime = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("XDG_RUNTIME_DIR"));
    const QString base = runtime.isEmpty()
                             ? QStringLiteral("/tmp/oxwm-%1").arg(getuid())
                             : runtime + QStringLiteral("/oxwm");
    const QString dir = base + QLatin1Char('/') + displayNumber();
    if (QFileInfo::exists(dir + QStringLiteral("/.socket.sock")))
        return dir;
    return QString();
}

bool available()
{
    return !socketDir().isEmpty();
}

QByteArray request(const QByteArray &command)
{
    const QString dir = socketDir();
    if (dir.isEmpty())
        return QByteArray();

    QLocalSocket sock;
    sock.connectToServer(dir + QStringLiteral("/.socket.sock"));
    if (!sock.waitForConnected(requestTimeoutMs))
        return QByteArray();

    /* The WM reads up to the newline, then answers. */
    sock.write(command + '\n');
    if (!sock.waitForBytesWritten(requestTimeoutMs))
        return QByteArray();

    /* oxwm writes the whole reply then closes, so read until the peer hangs
     * up rather than guessing at a length. */
    QByteArray out;
    while (sock.state() == QLocalSocket::ConnectedState) {
        if (!sock.waitForReadyRead(requestTimeoutMs))
            break;
        out += sock.readAll();
    }
    out += sock.readAll();
    return out;
}

QJsonDocument requestJson(const QByteArray &command)
{
    const QByteArray raw = request("j/" + command);
    if (raw.isEmpty())
        return QJsonDocument();
    return QJsonDocument::fromJson(raw);
}

QJsonArray requestArray(const QByteArray &command)
{
    return requestJson(command).array();
}

bool command(const QString &commands)
{
    const QByteArray reply = request(commands.toUtf8());
    if (reply.isEmpty())
        return false;
    /* One "ok" or "error: ..." line per command. */
    for (const QByteArray &line : reply.trimmed().split('\n'))
        if (line.trimmed() != "ok")
            return false;
    return true;
}

} /* namespace OxwmIpc */

/* ------------------------------------------------------------ event stream */

OxwmEventStream::OxwmEventStream(QObject *parent) : QObject(parent)
{
    connect(&m_sock, &QLocalSocket::readyRead, this,
            &OxwmEventStream::onReadyRead);
    connect(&m_sock, &QLocalSocket::connected, this, [this]() {
        m_buf.clear();
        emit connected();
    });
    /* oxwm restarting (or not up yet) is the normal case here, not an error
     * worth reporting -- just keep trying. */
    connect(&m_sock, &QLocalSocket::disconnected, this, [this]() {
        emit disconnected();
        QTimer::singleShot(reconnectMs, this, &OxwmEventStream::tryConnect);
    });
    connect(&m_sock, &QLocalSocket::errorOccurred, this, [this]() {
        QTimer::singleShot(reconnectMs, this, &OxwmEventStream::tryConnect);
    });
}

bool OxwmEventStream::isConnected() const
{
    return m_sock.state() == QLocalSocket::ConnectedState;
}

void OxwmEventStream::start()
{
    tryConnect();
}

void OxwmEventStream::tryConnect()
{
    if (m_sock.state() != QLocalSocket::UnconnectedState)
        return;
    const QString dir = OxwmIpc::socketDir();
    if (dir.isEmpty()) {
        QTimer::singleShot(reconnectMs, this, &OxwmEventStream::tryConnect);
        return;
    }
    m_sock.connectToServer(dir + QStringLiteral("/.socket2.sock"));
}

void OxwmEventStream::onReadyRead()
{
    m_buf += m_sock.readAll();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl);
        m_buf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const int sep = line.indexOf(">>");
        if (sep < 0) {
            emit wmEvent(QString::fromUtf8(line), QString());
            continue;
        }
        emit wmEvent(QString::fromUtf8(line.left(sep)),
                     QString::fromUtf8(line.mid(sep + 2)));
    }
}
