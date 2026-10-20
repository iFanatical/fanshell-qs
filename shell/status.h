/* Status the bar shows only when it has something to say: a filling disk,
 * keep awake / game mode being on, and network throughput.
 *
 *   Disk      / and /home (when separate), every five minutes; the pill
 *             shows while one is disk.warn percent full or more (default
 *             90). The System dropdown shows the fuller one either way.
 *   Modes     a pill while keep awake or game mode is on; click turns them
 *             off.
 *   Speed     download and upload rates over the physical interfaces
 *             (/proc/net/dev; VPN tunnels and bridges left out, as their
 *             traffic is counted again on the link), every two seconds, in
 *             the network pill. network.speed=off leaves it out. */
#ifndef FANSHELL_QS_STATUS_H
#define FANSHELL_QS_STATUS_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "procutil.h"
#include "widgets.h"

class DiskState : public QObject {
    Q_OBJECT
public:
    struct Mount {
        QString path;
        int percent = 0; /* used */
        qint64 freeBytes = 0;
    };

    static DiskState *instance();

    QVector<Mount> mounts() const { return m_mounts; }
    /* The fullest mount; percent -1 when none could be read. */
    Mount fullest() const;
    static int warnAt();
    void refresh();

signals:
    void changed();

private:
    explicit DiskState(QObject *parent = nullptr);

    QVector<Mount> m_mounts;
    QTimer m_timer;
};

class NetSpeed : public QObject {
    Q_OBJECT
public:
    static NetSpeed *instance();

    static bool enabled();
    /* Bytes per second over the last sample. */
    double down() const { return m_down; }
    double up() const { return m_up; }
    /* "1.2M", "340K", "0B": short enough for the bar. */
    static QString rate(double bytesPerSecond);

signals:
    void changed();

private:
    explicit NetSpeed(QObject *parent = nullptr);
    void sample();

    QTimer m_timer;
    QElapsedTimer m_clock;
    quint64 m_rx = 0, m_tx = 0;
    bool m_primed = false;
    double m_down = 0, m_up = 0;
};

class DiskWidget : public BarPill {
    Q_OBJECT
public:
    explicit DiskWidget(QWidget *parent = nullptr);

private:
    void sync();
};

class ModesWidget : public BarPill {
    Q_OBJECT
public:
    explicit ModesWidget(QWidget *parent = nullptr);

private:
    void sync();
};

#endif
