/* Minimal XDG .desktop database backing the launcher's "apps" mode:
 * scans XDG data dirs, exposes visible applications, and launches them with
 * Exec field-code handling and Terminal=true support. */
#ifndef FANSHELL_QS_DESKTOPENTRY_H
#define FANSHELL_QS_DESKTOPENTRY_H

#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

class DesktopEntry {
public:
    QString id;          /* desktop file id, e.g. org.gnome.Calculator */
    QString name;
    QString genericName;
    QString comment;
    QString icon;
    QString exec;
    QString workingDir;  /* Path= */
    bool terminal = false;
    bool noDisplay = false;

    void execute() const;
};

class DesktopEntries : public QObject {
    Q_OBJECT
public:
    static DesktopEntries *instance();

    const QVector<DesktopEntry> &applications() const { return m_apps; }

signals:
    void valuesChanged();

private:
    explicit DesktopEntries(QObject *parent = nullptr);
    void scan();
    void watchDirs(const QSet<QString> &dirs);

    QVector<DesktopEntry> m_apps;
    /* Live-reload: installs/uninstalls/edits change the .desktop database
     * while this (long-running) process is up, unlike a one-shot launcher.
     * Directory-level watch catches add/remove/rename; debounced so a
     * package manager dropping many files at once triggers one rescan. */
    QFileSystemWatcher m_watcher;
    QTimer m_rescanTimer;
};

#endif
