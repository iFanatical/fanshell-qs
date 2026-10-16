/* Bluetooth: adapter/device state, the bar tile, and its control popup. */
#ifndef FANSHELL_QS_BLUETOOTH_H
#define FANSHELL_QS_BLUETOOTH_H

#include "popup.h"
#include "procutil.h"
#include "widgets.h"

/* Bluetooth state via the fanshell-qs-bt bridge (bluetoothctl). Singleton:
 * exactly one watch process, no matter how many bars use it. */
class BluetoothState : public QObject {
    Q_OBJECT
public:
    struct Device {
        QString mac, name;
        bool connected = false, paired = false;
    };

    static BluetoothState *instance();

    bool powered = false;
    bool discovering = false;
    QVector<Device> devices;

    int connectedCount() const
    {
        int n = 0;
        for (const Device &d : devices)
            if (d.connected)
                n++;
        return n;
    }

    void togglePower();
    void scan();
    void connectDevice(const QString &mac);
    void disconnectDevice(const QString &mac);
    void setPolling(bool enabled);

signals:
    void changed();

private:
    explicit BluetoothState(QObject *parent = nullptr);
    void parse(const QString &block);
    void action(const QStringList &args);

    BlockWatchProcess m_watch;
    CollectorProcess m_action;
    bool m_polling = false;
};

class BluetoothPopup;

class BluetoothWidget : public BarPill {
    Q_OBJECT
public:
    explicit BluetoothWidget(QWidget *parent = nullptr);

private:
    void sync();

    BluetoothPopup *m_popup;
};

class BluetoothPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit BluetoothPopup(QWidget *anchor);

private:
    void rebuild();

    ShellButton *m_powerBtn;
    ShellButton *m_scanBtn;
    QWidget *m_listBox;
};

#endif
