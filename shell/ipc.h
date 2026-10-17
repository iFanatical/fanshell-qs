/* Control socket, driven by `fanshell-qs-shell ipc call <target> <fn>` --
 * this is what a window-manager or sxhkd key binding invokes to raise the
 * launcher:
 *   fanshell-qs-shell ipc call launcher toggle|show|hide
 *   fanshell-qs-shell ipc call runner   toggle|show|hide
 *   fanshell-qs-shell ipc call notifications toggle-dnd
 *   fanshell-qs-shell ipc call media play-pause|previous|next
 *   fanshell-qs-shell ipc call shell quit
 *   fanshell-qs-shell ipc call wm changed      (qtile's hooks; see qtilestate.h)
 * A QLocalServer in $XDG_RUNTIME_DIR, keyed by $DISPLAY, readable by its
 * owner only. */
#ifndef FANSHELL_QS_IPC_H
#define FANSHELL_QS_IPC_H

#include <QLocalServer>
#include <QMap>
#include <QObject>

#include <functional>

class IpcServer : public QObject {
    Q_OBJECT
public:
    explicit IpcServer(QObject *parent = nullptr);

    bool listen();
    void handle(const QString &target, const QString &function,
                std::function<void()> fn);

    static QString socketPath();
    /* Client side; returns process exit code. */
    static int call(const QString &target, const QString &function);
    static bool serverRunning();

private:
    QLocalServer m_server;
    QMap<QString, QMap<QString, std::function<void()>>> m_handlers;
};

#endif
