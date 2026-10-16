/* XEmbed system-tray manager for applications that predate
 * StatusNotifierItem (Wine, older X11 tools). Owns _NET_SYSTEM_TRAY_S0 --
 * which is free as long as the window manager's own bar and systray are off
 * (oxwm's bar.set_position("none"), no Systray widget in qtile) -- and shows each docked icon as an item in the SNI tray. */
#ifndef FANSHELL_QS_XEMBEDTRAY_H
#define FANSHELL_QS_XEMBEDTRAY_H

#include <QHash>
#include <QObject>

class QSocketNotifier;
class SniItem;
struct _XDisplay;

class XEmbedTray : public QObject {
    Q_OBJECT
public:
    static XEmbedTray *instance();
    ~XEmbedTray() override;

    bool active() const { return m_display != nullptr; }
    void sendButton(unsigned long window, int button);

private:
    struct Entry {
        SniItem *item = nullptr;
        unsigned long damage = 0;
    };

    explicit XEmbedTray(QObject *parent = nullptr);
    void processEvents();
    void dock(unsigned long window);
    void remove(unsigned long window, bool windowDestroyed = false);
    void refresh(unsigned long window);
    void announceManager();
    unsigned long atom(const char *name);

    _XDisplay *m_display = nullptr;
    unsigned long m_root = 0;
    unsigned long m_manager = 0;
    unsigned long m_selection = 0;
    unsigned long m_managerAtom = 0;
    unsigned long m_opcodeAtom = 0;
    unsigned long m_xembedAtom = 0;
    unsigned long m_netWmNameAtom = 0;
    unsigned long m_utf8Atom = 0;
    int m_damageEvent = 0;
    QSocketNotifier *m_notifier = nullptr;
    QHash<unsigned long, Entry> m_entries;
};

#endif
