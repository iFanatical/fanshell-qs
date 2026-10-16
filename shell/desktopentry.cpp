#include "desktopentry.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <csignal>

/* Strip %f/%F/%u/%U/... field codes; %% -> %. */
static QString stripFieldCodes(const QString &exec)
{
    QString out;
    out.reserve(exec.size());
    for (int i = 0; i < exec.size(); i++) {
        if (exec[i] == QLatin1Char('%') && i + 1 < exec.size()) {
            const QChar c = exec[i + 1];
            if (c == QLatin1Char('%')) {
                out += QLatin1Char('%');
                i++;
                continue;
            }
            /* drop the code */
            i++;
            continue;
        }
        out += exec[i];
    }
    return out;
}

/* Tokenize an Exec line: space-separated, double quotes group, backslash
 * escapes inside quotes. */
static QStringList tokenize(const QString &s)
{
    QStringList args;
    QString cur;
    bool inQuote = false, any = false;
    for (int i = 0; i < s.size(); i++) {
        const QChar c = s[i];
        if (inQuote) {
            if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                cur += s[++i];
            } else if (c == QLatin1Char('"')) {
                inQuote = false;
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuote = true;
            any = true;
        } else if (c.isSpace()) {
            if (!cur.isEmpty() || any) {
                args << cur;
                cur.clear();
                any = false;
            }
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty() || any)
        args << cur;
    return args;
}

void DesktopEntry::execute() const
{
    QStringList argv = tokenize(stripFieldCodes(exec).trimmed());
    if (argv.isEmpty())
        return;
    if (terminal) {
        QString term = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("TERMINAL"));
        if (term.isEmpty())
            term = QStringLiteral("xterm");
        argv = QStringList{term, QStringLiteral("-e")} + argv;
    }
    const QString program = argv.takeFirst();

    /* fanshell-qs-shell is long-running and repeatedly uses QProcess for its own
     * polling widgets; a launched program that itself forks+waitpid()s a
     * helper at startup (an AppImage mounting itself via FUSE, notably)
     * should never be able to observe whatever signal dispositions this
     * process has accumulated. Force SIGCHLD/SIGINT/SIGQUIT back to default
     * in the child before exec(), regardless of our own current state. */
    auto *proc = new QProcess;
    proc->setProgram(program);
    proc->setArguments(argv);
    if (!workingDir.isEmpty())
        proc->setWorkingDirectory(workingDir);
    proc->setChildProcessModifier([]() {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
    });
    const bool ok = proc->startDetached();
    if (!ok)
        qWarning() << "DesktopEntry::execute: failed to start" << program
                   << "(id=" << id << ")";
    proc->deleteLater();
}

DesktopEntries *DesktopEntries::instance()
{
    static DesktopEntries *s = new DesktopEntries();
    return s;
}

DesktopEntries::DesktopEntries(QObject *parent) : QObject(parent)
{
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(300); /* coalesce a burst of fs events */
    connect(&m_rescanTimer, &QTimer::timeout, this, &DesktopEntries::scan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_rescanTimer,
            qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_rescanTimer,
            qOverload<>(&QTimer::start));
    scan();
}

void DesktopEntries::watchDirs(const QSet<QString> &dirs)
{
    QStringList toAdd;
    for (const QString &d : dirs)
        if (!m_watcher.directories().contains(d))
            toAdd << d;
    if (!toAdd.isEmpty())
        m_watcher.addPaths(toAdd);
}

void DesktopEntries::scan()
{
    QStringList dirs = QStandardPaths::standardLocations(
        QStandardPaths::ApplicationsLocation);
    QHash<QString, bool> seen;
    QVector<DesktopEntry> apps;
    QSet<QString> liveDirs;

    for (const QString &dir : dirs) {
        if (QDir(dir).exists())
            liveDirs.insert(dir);
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            liveDirs.insert(QFileInfo(file).absolutePath());
            QString id = QDir(dir).relativeFilePath(file);
            id.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (seen.contains(id))
                continue; /* earlier dirs take precedence */
            seen.insert(id, true);

            QFile f(file);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            DesktopEntry e;
            e.id = id;
            bool inEntry = false;
            bool hidden = false;
            QString type;
            while (!f.atEnd()) {
                const QString line =
                    QString::fromUtf8(f.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    inEntry = (line == QLatin1String("[Desktop Entry]"));
                    continue;
                }
                if (!inEntry || line.isEmpty()
                        || line.startsWith(QLatin1Char('#')))
                    continue;
                const int eq = line.indexOf(QLatin1Char('='));
                if (eq < 0)
                    continue;
                const QString k = line.left(eq).trimmed();
                const QString v = line.mid(eq + 1).trimmed();
                if (k == QLatin1String("Type"))
                    type = v;
                else if (k == QLatin1String("Name"))
                    e.name = v;
                else if (k == QLatin1String("GenericName"))
                    e.genericName = v;
                else if (k == QLatin1String("Comment"))
                    e.comment = v;
                else if (k == QLatin1String("Icon"))
                    e.icon = v;
                else if (k == QLatin1String("Exec"))
                    e.exec = v;
                else if (k == QLatin1String("Path"))
                    e.workingDir = v;
                else if (k == QLatin1String("Terminal"))
                    e.terminal = (v == QLatin1String("true"));
                else if (k == QLatin1String("NoDisplay"))
                    e.noDisplay = (v == QLatin1String("true"));
                else if (k == QLatin1String("Hidden"))
                    hidden = (v == QLatin1String("true"));
            }
            if (type != QLatin1String("Application") || hidden
                    || e.exec.isEmpty())
                continue;
            apps.push_back(e);
        }
    }

    m_apps = apps;
    watchDirs(liveDirs);
    emit valuesChanged();
}
