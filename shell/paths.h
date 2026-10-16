/* Where the shell keeps what it should remember: $XDG_CONFIG_HOME/fanshell-qs
 * (normally ~/.config/fanshell-qs). Everything persistent goes here, never
 * under ~/.cache, so clearing the cache loses nothing the user chose:
 *
 *   wallpaper.conf   the picker's last choice, restored at startup
 *   vibrance.conf    digital vibrance per output, restored at startup
 *   outputs.conf     output layouts, written via fanshell-qs-outputs
 *   dpms.conf        screen blanking and DPMS, applied at startup
 *   lock.conf        lock on idle and before sleep
 *   websites.json    the menu's Websites page (hand-edited)
 *   sounds/          optional volume-change sound
 *
 * Runtime-only state (sockets, scratchpad window ids) belongs in
 * $XDG_RUNTIME_DIR instead, because it means nothing after a reboot. */
#ifndef FANSHELL_QS_PATHS_H
#define FANSHELL_QS_PATHS_H

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace Paths {

inline QString configDir()
{
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/fanshell-qs");
}

inline QString configFile(const QString &name)
{
    return configDir() + QLatin1Char('/') + name;
}

/* KEY=VALUE files: one pair per line, '#' comments, blank lines ignored. */
inline QHash<QString, QString> readConf(const QString &name)
{
    QHash<QString, QString> out;
    QFile file(configFile(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    for (const QString &raw : QString::fromUtf8(file.readAll()).split('\n')) {
        const QString line = raw.trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || eq <= 0)
            continue;
        out.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return out;
}

/* Replaces the file atomically, so a crash mid-write cannot leave it empty.
 * `lines` are written in order after the `header` comment. */
inline bool writeConf(const QString &name, const QString &header,
                      const QStringList &lines)
{
    QDir().mkpath(configDir());
    QSaveFile file(configFile(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QString text;
    for (const QString &h : header.split('\n'))
        text += QStringLiteral("# ") + h + QLatin1Char('\n');
    for (const QString &l : lines)
        text += l + QLatin1Char('\n');
    file.write(text.toUtf8());
    return file.commit();
}

} /* namespace Paths */

#endif
