/* qtile IPC transport.
 *
 * qtile serves its command interface (what `qtile cmd-obj` uses) on a unix
 * socket, $XDG_CACHE_HOME/qtile/qtilesocket.$DISPLAY (~/.cache when unset).
 * One command per connection: the client writes a JSON request and closes
 * its write side, and qtile answers with a JSON reply and hangs up.
 *
 *   request  [selectors, command, args, kwargs, lifted]
 *            selectors is a path of [object, id] pairs from the root, e.g.
 *            [["group", "3"]]; [] is the root object. lifted is true:
 *            qtile converts args by the command's type annotations.
 *   reply    [status, result], status 0 on success.
 *
 * qtile has no event stream; see qtilestate.h for how the shell learns of
 * changes. */
#ifndef FANSHELL_QS_QTILEIPC_H
#define FANSHELL_QS_QTILEIPC_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace QtileIpc {

QString socketPath();
/* The socket exists; qtile may still not answer (mid-restart). */
bool available();

/* Synchronous command. qtile answers from its event loop, so the block is a
 * few milliseconds. *ok is false on a transport failure or a non-zero
 * status; the result is then the error text, if any. */
QJsonValue call(const QJsonArray &selectors, const QString &command,
                const QJsonArray &args = QJsonArray(), bool *ok = nullptr);

/* The root object's eval(code), which answers str() of the value. */
QString eval(const QString &code, bool *ok = nullptr);

} /* namespace QtileIpc */

#endif
