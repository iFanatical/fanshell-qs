#include "ipc.h"

#include <QDir>
#include <QLocalSocket>
#include <QProcessEnvironment>

QString IpcServer::socketPath()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString dir = env.value(QStringLiteral("XDG_RUNTIME_DIR"));
    if (dir.isEmpty())
        dir = QDir::tempPath();
    /* Keyed per X display so two sessions on one machine (say, a nested
     * Xephyr while testing) don't fight over the socket. */
    QString key = env.value(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char(':'), QLatin1Char('x'));
    return dir + QStringLiteral("/fanshell-qs-shell-") + key
           + QStringLiteral(".sock");
}

IpcServer::IpcServer(QObject *parent) : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *sock = m_server.nextPendingConnection()) {
            connect(sock, &QLocalSocket::readyRead, sock, [this, sock]() {
                if (!sock->canReadLine())
                    return;
                const QString line =
                    QString::fromUtf8(sock->readLine()).trimmed();
                const QStringList parts =
                    line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                bool ok = false;
                if (parts.size() >= 2) {
                    const auto t = m_handlers.constFind(parts[0]);
                    if (t != m_handlers.constEnd()) {
                        const auto f = t->constFind(parts[1]);
                        if (f != t->constEnd()) {
                            (*f)();
                            ok = true;
                        }
                    }
                }
                sock->write(ok ? "ok\n" : "error\n");
                sock->flush();
                sock->disconnectFromServer();
            });
            connect(sock, &QLocalSocket::disconnected, sock,
                    &QLocalSocket::deleteLater);
        }
    });
}

bool IpcServer::listen()
{
    const QString path = socketPath();
    if (serverRunning())
        return false;
    QLocalServer::removeServer(path); /* stale socket */
    /* Owner-only. $XDG_RUNTIME_DIR is already 0700, but the /tmp fallback is
     * shared, and anyone who can connect can quit the shell, toggle the VPN,
     * or drive the power menu. */
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    return m_server.listen(path);
}

void IpcServer::handle(const QString &target, const QString &function,
                       std::function<void()> fn)
{
    m_handlers[target][function] = std::move(fn);
}

bool IpcServer::serverRunning()
{
    QLocalSocket sock;
    sock.connectToServer(socketPath());
    return sock.waitForConnected(200);
}

int IpcServer::call(const QString &target, const QString &function)
{
    QLocalSocket sock;
    sock.connectToServer(socketPath());
    if (!sock.waitForConnected(1000)) {
        fprintf(stderr, "fanshell-qs-shell: no running instance (socket %s)\n",
                qPrintable(socketPath()));
        return 1;
    }
    sock.write((target + QLatin1Char(' ') + function + QLatin1Char('\n'))
                   .toUtf8());
    sock.flush();
    if (!sock.waitForReadyRead(2000))
        return 1;
    const QByteArray reply = sock.readAll().trimmed();
    return reply == "ok" ? 0 : 1;
}
