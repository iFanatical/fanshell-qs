/* oxwm IPC transport.
 *
 * Upstream oxwm has no IPC; fanshell-qs adds one (oxwm-ipc/ in this tree,
 * built by scripts/build-patched-oxwm). It is modelled on Hyprland's, so the
 * shell keeps the shape it had under Hyprland: two unix sockets in
 * $XDG_RUNTIME_DIR/oxwm/<display number>/,
 *
 *   .socket.sock   request/response -- write one line, read the reply, the
 *                  WM closes the connection. `j/state` and `j/clients` answer
 *                  in JSON; `dispatch <action> [arg]` runs any keybind action
 *                  by its config name. Commands may be joined with ';'.
 *   .socket2.sock  a push event stream: one "EVENT>>DATA\n" line per change.
 *
 * See oxwm-ipc/src/ipc.zig for the full command and event list. */
#ifndef FANSHELL_QS_OXWMIPC_H
#define FANSHELL_QS_OXWMIPC_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QObject>
#include <QString>

namespace OxwmIpc {

/* Directory holding the two sockets, or empty when no patched oxwm is
 * listening on this $DISPLAY. */
QString socketDir();
bool available();

/* Synchronous request on .socket.sock. oxwm answers from its event loop and
 * hangs up, so the block is on the order of microseconds; returns empty on
 * any failure. */
QByteArray request(const QByteArray &command);

/* request("j/" + command) parsed as JSON; returns an empty document/array on
 * failure so callers can iterate the result unconditionally. */
QJsonDocument requestJson(const QByteArray &command);
QJsonArray requestArray(const QByteArray &command);

/* Run commands, e.g. "focusmonitor 1;dispatch view_tag 2". True when every
 * command answered "ok". */
bool command(const QString &commands);

} /* namespace OxwmIpc */

/* Persistent .socket2.sock reader. Emits one wmEvent() per line and
 * reconnects on its own if the window manager restarts under us. */
class OxwmEventStream : public QObject {
    Q_OBJECT
public:
    explicit OxwmEventStream(QObject *parent = nullptr);

    void start();
    bool isConnected() const;

signals:
    /* name/data split on the ">>" separator; data may be empty. Not called
     * `event`: that would hide QObject::event(QEvent *). */
    void wmEvent(const QString &name, const QString &data);
    void connected();
    void disconnected();

private:
    void tryConnect();
    void onReadyRead();

    QLocalSocket m_sock;
    QByteArray m_buf;
};

#endif
