/* Network state, the bar widget, and the Wi-Fi manager popup. */
#ifndef FANSHELL_QS_NETWORK_H
#define FANSHELL_QS_NETWORK_H

#include <QProcess>

#include "popup.h"
#include "procutil.h"
#include "widgets.h"

/* Network state via the backend-adaptive fanshell-qs-net bridge.
 *   backend == "nm" -> NetworkManager: Wi-Fi list/connect/scan/radio.
 *   backend == "ip" -> custom bridge/VLAN desktop: link status + re-apply.
 * Singleton: exactly one watch process, no matter how many bars use it. */
class NetworkState : public QObject {
    Q_OBJECT
public:
    struct Link { QString name, state, type, addr; };
    struct Wifi { QString ssid; int signal = 0; QString security; bool active = false; };

    static NetworkState *instance();

    QString backend = QStringLiteral("ip");
    QString device;
    QString gateway;

    bool wifiEnabled = false;
    QString connectivity;
    QString kind = QStringLiteral("none"); /* wifi | ethernet | vpn | none */
    QString connName;
    QString ssid;
    int wifiSignal = 0;
    QString security;
    QString ip4;

    QVector<Link> links;
    QVector<Wifi> wifiNetworks;
    bool scanning = false;

    bool online() const
    {
        return connectivity == QLatin1String("full")
               || (backend == QLatin1String("ip") && !gateway.isEmpty());
    }
    bool isNm() const { return backend == QLatin1String("nm"); }

    void refreshWifi();
    void wifiScan();
    void wifiConnect(const QString &ssid, const QString &pw);
    void wifiDisconnect();
    void wifiForget(const QString &ssid);
    void setRadio(bool on);
    void reapply();
    void linkUp(const QString &n);
    void linkDown(const QString &n);

signals:
    void changed();
    void wifiListChanged();

private:
    explicit NetworkState(QObject *parent = nullptr);
    void parseStatus(const QString &block);
    void parseWifi(const QString &text);
    void action(const QStringList &args);

    BlockWatchProcess m_watch;
    CollectorProcess m_wifiList;
    QProcess m_scan;
    QProcess m_action;
    CollectorProcess m_signal;
};

class NetworkPopup;

class NetworkWidget : public BarPill {
    Q_OBJECT
public:
    explicit NetworkWidget(QWidget *parent = nullptr);

private:
    void sync();

    NetworkPopup *m_popup;
};

class NetworkPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit NetworkPopup(QWidget *anchor);

    void openPopup() override;

private:
    /* Header buttons + summary: cheap, synced on every state change. */
    void syncControls();
    /* Row list: rebuilt only when its content actually changes (wifi-list
     * results, backend flip, link changes, password-row expand), so a status
     * refresh cannot destroy a password field mid-typing. */
    void rebuildList();

    QString m_selectedSsid; /* wifi row expanded for password entry */
    bool m_lastNm = false;
    QStringList m_lastLinksKey;
    TextItem *m_headerTitle;
    ShellButton *m_radioBtn;
    ShellButton *m_scanBtn;
    ShellButton *m_reapplyBtn;
    TextItem *m_summary;
    TextItem *m_summarySub;
    QWidget *m_listBox;      /* wifi rows / link rows / empty text */
};

#endif
