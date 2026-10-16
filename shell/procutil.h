/* QProcess helpers for the script bridges:
 *   CollectorProcess — Process + StdioCollector (run, buffer stdout, emit)
 *   BlockWatchProcess — Process + SplitParser (long-lived, "\n\n" blocks,
 *                       respawned with backoff if it exits on its own)
 *
 * Both reap: every helper gets its own process group plus PR_SET_PDEATHSIG.
 * Clean shutdown signals the whole group, while PDEATHSIG reaches the group
 * leader after a crash/SIGKILL. Long-running scripts must trap that SIGTERM
 * and relay it to their group when they create grandchildren.
 *
 * Programs the user *launches* (desktopentry, the launcher, power actions)
 * deliberately do not go through here: those must outlive the bar. */
#ifndef FANSHELL_QS_PROCUTIL_H
#define FANSHELL_QS_PROCUTIL_H

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

namespace ProcUtil {
/* Apply the die-with-parent child setup to a QProcess. */
void reapWithParent(QProcess *proc);
/* Terminate every helper still running; wired to QCoreApplication::aboutToQuit
 * so a clean exit doesn't have to rely on the kernel's PDEATHSIG. */
void terminateAll();
} /* namespace ProcUtil */

/* One-shot command; collect stdout; `finished(text)` when it exits. Starting
 * while already running is a no-op (QML `running = true` semantics). */
class CollectorProcess : public QObject {
    Q_OBJECT
public:
    explicit CollectorProcess(QObject *parent = nullptr);

    void setCommand(const QStringList &cmd) { m_cmd = cmd; }
    void start();
    void start(const QStringList &cmd);

signals:
    void finished(const QString &stdoutText);

private:
    QProcess m_proc;
    QStringList m_cmd;
};

/* Long-running watcher; stdout split on a marker (default "\n\n"), each chunk
 * emitted via block(). */
class BlockWatchProcess : public QObject {
    Q_OBJECT
public:
    explicit BlockWatchProcess(const QStringList &cmd,
                               const QByteArray &marker = "\n\n",
                               QObject *parent = nullptr);

    void start();
    void stop();

signals:
    void block(const QString &data);

private:
    void onReadyRead();
    void scheduleRestart();

    QProcess m_proc;
    QStringList m_cmd;
    QByteArray m_marker;
    QByteArray m_buf;
    /* Restarted with backoff if it exits while still wanted. */
    QTimer m_restart;
    bool m_wanted = false;
    int m_backoffMs = 1000;
    qint64 m_startedAt = 0;
};

#endif
