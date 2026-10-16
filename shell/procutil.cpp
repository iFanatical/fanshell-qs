#include "procutil.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QList>
#include <QTimer>

#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
QList<QPointer<QProcess>> g_helpers;
bool g_hookInstalled = false;
/* Set once shutdown begins, so watchers we are killing don't respawn. */
bool g_quitting = false;

/* Watcher respawn backoff: quick first retry, capped so a helper that can
 * never start costs one attempt every half minute. */
constexpr int restartMinMs = 1000;
constexpr int restartMaxMs = 30000;
/* A watcher that lived at least this long was healthy; reset the backoff. */
constexpr qint64 healthyRunMs = 30000;
} /* namespace */

namespace ProcUtil {

void reapWithParent(QProcess *proc)
{
    proc->setChildProcessModifier([]() {
        /* Keep helper descendants out of the shell's process group so cleanup
         * can safely address the complete helper tree. */
        setpgid(0, 0);
        /* Kernel-enforced: when the thread that forked us dies -- cleanly,
         * crashed, or SIGKILLed -- the child gets SIGTERM. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        /* The parent could already have died between fork() and here, in
         * which case the disposition above will never fire. */
        if (getppid() == 1)
            _exit(1);
    });

    g_helpers.append(proc);
    if (!g_hookInstalled && qApp) {
        g_hookInstalled = true;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp,
                         []() { terminateAll(); });
    }
}

void terminateAll()
{
    g_quitting = true;
    for (const QPointer<QProcess> &p : std::as_const(g_helpers)) {
        if (!p || p->state() == QProcess::NotRunning)
            continue;
        const qint64 pid = p->processId();
        if (pid > 1)
            kill(-pid, SIGTERM);
        else
            p->terminate();
        if (!p->waitForFinished(300)) {
            if (pid > 1)
                kill(-pid, SIGKILL);
            else
                p->kill();
        }
    }
    g_helpers.clear();
}

} /* namespace ProcUtil */

CollectorProcess::CollectorProcess(QObject *parent) : QObject(parent)
{
    ProcUtil::reapWithParent(&m_proc);
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_proc, &QProcess::finished, this, [this]() {
        emit finished(QString::fromUtf8(m_proc.readAllStandardOutput()));
    });
}

void CollectorProcess::start()
{
    if (m_cmd.isEmpty() || m_proc.state() != QProcess::NotRunning)
        return;
    m_proc.start(m_cmd.first(), m_cmd.mid(1));
}

void CollectorProcess::start(const QStringList &cmd)
{
    if (m_proc.state() != QProcess::NotRunning)
        return;
    m_cmd = cmd;
    start();
}

BlockWatchProcess::BlockWatchProcess(const QStringList &cmd,
                                     const QByteArray &marker, QObject *parent)
    : QObject(parent), m_cmd(cmd), m_marker(marker)
{
    ProcUtil::reapWithParent(&m_proc);
    connect(&m_proc, &QProcess::readyReadStandardOutput, this,
            &BlockWatchProcess::onReadyRead);

    /* A watcher's job is to run for the life of the shell. If it exits on its
     * own -- NetworkManager restarted under `nmcli monitor`, wl-paste lost
     * its compositor connection -- bring it back rather than leave the
     * widget frozen on its last state. */
    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, &BlockWatchProcess::start);
    connect(&m_proc, &QProcess::finished, this,
            &BlockWatchProcess::scheduleRestart);
    connect(&m_proc, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart)
                    scheduleRestart();
            });
}

void BlockWatchProcess::start()
{
    m_wanted = true;
    m_restart.stop();
    if (m_cmd.isEmpty() || m_proc.state() != QProcess::NotRunning)
        return;
    m_buf.clear();
    m_startedAt = QDateTime::currentMSecsSinceEpoch();
    m_proc.start(m_cmd.first(), m_cmd.mid(1));
}

void BlockWatchProcess::scheduleRestart()
{
    if (!m_wanted || g_quitting || m_restart.isActive())
        return;
    if (QDateTime::currentMSecsSinceEpoch() - m_startedAt >= healthyRunMs)
        m_backoffMs = restartMinMs;
    m_restart.start(m_backoffMs);
    m_backoffMs = qMin(m_backoffMs * 2, restartMaxMs);
}

void BlockWatchProcess::stop()
{
    m_wanted = false;
    m_restart.stop();
    if (m_proc.state() == QProcess::NotRunning)
        return;
    const qint64 pid = m_proc.processId();
    if (pid > 1)
        kill(-pid, SIGTERM);
    else
        m_proc.terminate();
    if (!m_proc.waitForFinished(300)) {
        if (pid > 1)
            kill(-pid, SIGKILL);
        else
            m_proc.kill();
        m_proc.waitForFinished(300);
    }
    m_buf.clear();
}

void BlockWatchProcess::onReadyRead()
{
    m_buf += m_proc.readAllStandardOutput();
    int idx;
    while ((idx = m_buf.indexOf(m_marker)) >= 0) {
        const QByteArray chunk = m_buf.left(idx);
        m_buf.remove(0, idx + m_marker.size());
        if (!chunk.trimmed().isEmpty())
            emit block(QString::fromUtf8(chunk));
    }
}
