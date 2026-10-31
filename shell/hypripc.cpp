#include "hypripc.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTimer>

namespace {

/* Reconnect backoff for the event stream (compositor restart, or the shell
 * having been started before Hyprland finished coming up). */
constexpr int reconnectMs = 1000;
/* Generous relative to a local socket round trip, small enough that a wedged
 * compositor can't freeze the bar for a visible beat. */
constexpr int requestTimeoutMs = 500;

QString instanceSignature()
{
    return QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("HYPRLAND_INSTANCE_SIGNATURE"));
}

} /* namespace */

namespace HyprIpc {

QString socketDir()
{
    const QString sig = instanceSignature();
    if (sig.isEmpty())
        return QString();

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString runtime = env.value(QStringLiteral("XDG_RUNTIME_DIR"));

    QStringList candidates;
    if (!runtime.isEmpty())
        candidates << runtime + QStringLiteral("/hypr/") + sig;
    /* Hyprland < 0.40 put them here; harmless to probe on newer builds. */
    candidates << QStringLiteral("/tmp/hypr/") + sig;

    for (const QString &dir : candidates)
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

    sock.write(command);
    if (!sock.waitForBytesWritten(requestTimeoutMs))
        return QByteArray();

    /* Hyprland writes the whole reply then closes, so read until the peer
     * hangs up rather than guessing at a length. */
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

namespace {
/* Which dispatch spelling this compositor accepted; see hypripc.h. */
enum class Dialect { Unknown, Lua, Legacy };
Dialect g_dialect = Dialect::Unknown;

bool tryDispatch(const QString &payload)
{
    /* Hyprland answers "ok" on success and an error string otherwise. */
    return request("dispatch " + payload.toUtf8()).trimmed() == "ok";
}
} /* namespace */

bool dispatch(const QString &luaForm, const QString &legacyForm)
{
    switch (g_dialect) {
    case Dialect::Lua:
        return tryDispatch(luaForm);
    case Dialect::Legacy:
        return tryDispatch(legacyForm);
    case Dialect::Unknown:
        break;
    }
    if (tryDispatch(luaForm)) {
        g_dialect = Dialect::Lua;
        return true;
    }
    if (tryDispatch(legacyForm)) {
        g_dialect = Dialect::Legacy;
        return true;
    }
    return false;
}

namespace {

bool okReplies(const QByteArray &reply)
{
    /* A batch answers one "ok" per command, separated by blank lines. */
    const QList<QByteArray> parts = reply.split('\n');
    bool any = false;
    for (const QByteArray &p : parts) {
        const QByteArray t = p.trimmed();
        if (t.isEmpty())
            continue;
        if (t != "ok")
            return false;
        any = true;
    }
    return any;
}

/* Hyprland's config backend. The classic hyprlang one takes `keyword`; the
 * Lua one (0.56) rejects that and wants `eval hl.config{...}`. A Lua comment
 * is a no-op to the one and an error to the other (as hyprvibrance probes). */
bool luaConfig()
{
    static int cached = -1;
    if (cached < 0) {
        const QByteArray reply = request("eval -- fanshell-qs backend probe");
        if (reply.isEmpty())
            return false; /* unreachable: ask again next time */
        cached = reply.contains("only supported with the lua") ? 0 : 1;
    }
    return cached == 1;
}

QString classicValue(const QVariant &v)
{
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    const QString s = v.toString();
    /* hyprlang needs a sentinel to clear a string option. */
    return s.isEmpty() ? QStringLiteral("[[EMPTY]]") : s;
}

QString luaStatement(const QString &path, const QVariant &value)
{
    const QStringList parts = path.split(QLatin1Char(':'));
    QString lua = luaValue(value);
    for (int i = parts.size() - 1; i >= 0; --i)
        lua = QStringLiteral("{ %1 = %2 }").arg(parts[i], lua);
    return QStringLiteral("hl.config(%1)").arg(lua);
}

} /* namespace */

bool keyword(const QString &option, const QString &value, const QString &lua)
{
    const bool ok = !lua.isEmpty() && luaConfig()
                        ? okReplies(request("eval " + lua.toUtf8()))
                        : okReplies(request("keyword " + option.toUtf8() + ' '
                                            + value.toUtf8()));
    if (!ok)
        qWarning("fanshell-qs: Hyprland refused to set %s=%s", qPrintable(option),
                 qPrintable(value));
    return ok;
}

QString luaString(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QString luaValue(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        return QString::number(v.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float:
        return QString::number(v.toDouble(), 'g', 10);
    default:
        return luaString(v.toString());
    }
}

bool setOption(const QString &path, const QVariant &value)
{
    return setOptions({{path, value}});
}

bool setOptions(const QVector<QPair<QString, QVariant>> &options)
{
    if (options.isEmpty())
        return true;
    QByteArray cmd;
    if (luaConfig()) {
        /* Statements in one chunk are evaluated together: one config
         * refresh for all of them. */
        QStringList stmts;
        for (const auto &o : options)
            stmts << luaStatement(o.first, o.second);
        cmd = "eval " + stmts.join(QLatin1Char(' ')).toUtf8();
    } else {
        QStringList cmds;
        for (const auto &o : options)
            cmds << QStringLiteral("keyword %1 %2").arg(o.first, classicValue(o.second));
        cmd = "[[BATCH]]" + cmds.join(QLatin1Char(';')).toUtf8();
    }
    const bool ok = okReplies(request(cmd));
    if (!ok)
        qWarning("fanshell-qs: Hyprland refused %s", cmd.constData());
    return ok;
}

QVariant optionValue(const QString &path)
{
    /* The text form ("int: 2", "float: 1.000000", "str: /a/b"): the one
     * known to answer the same under both config backends. */
    const QString reply = QString::fromUtf8(request("getoption " + path.toUtf8()));
    for (const QString &line : reply.split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1String("int: ")))
            return t.mid(5).toLongLong();
        if (t.startsWith(QLatin1String("float: ")))
            return t.mid(7).toDouble();
        if (t.startsWith(QLatin1String("str: "))) {
            const QString v = t.mid(5);
            return v == QLatin1String("[[EMPTY]]") ? QString() : v;
        }
    }
    return QVariant();
}

} /* namespace HyprIpc */

/* ------------------------------------------------------------ event stream */

HyprEventStream::HyprEventStream(QObject *parent) : QObject(parent)
{
    connect(&m_sock, &QLocalSocket::readyRead, this,
            &HyprEventStream::onReadyRead);
    connect(&m_sock, &QLocalSocket::connected, this, [this]() {
        m_buf.clear();
        emit connected();
    });
    /* Hyprland restarting (or not up yet) is the normal case here, not an
     * error worth reporting -- just keep trying. */
    connect(&m_sock, &QLocalSocket::disconnected, this, [this]() {
        QTimer::singleShot(reconnectMs, this, &HyprEventStream::tryConnect);
    });
    connect(&m_sock, &QLocalSocket::errorOccurred, this, [this]() {
        QTimer::singleShot(reconnectMs, this, &HyprEventStream::tryConnect);
    });
}

bool HyprEventStream::isConnected() const
{
    return m_sock.state() == QLocalSocket::ConnectedState;
}

void HyprEventStream::start()
{
    tryConnect();
}

void HyprEventStream::tryConnect()
{
    if (m_sock.state() != QLocalSocket::UnconnectedState)
        return;
    const QString dir = HyprIpc::socketDir();
    if (dir.isEmpty()) {
        QTimer::singleShot(reconnectMs, this, &HyprEventStream::tryConnect);
        return;
    }
    m_sock.connectToServer(dir + QStringLiteral("/.socket2.sock"));
}

void HyprEventStream::onReadyRead()
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
            emit hyprEvent(QString::fromUtf8(line), QString());
            continue;
        }
        emit hyprEvent(QString::fromUtf8(line.left(sep)),
                       QString::fromUtf8(line.mid(sep + 2)));
    }
}
