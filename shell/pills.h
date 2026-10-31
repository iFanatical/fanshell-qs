/* Script-backed bar pills and singleton state helpers. */
#ifndef FANSHELL_QS_PILLS_H
#define FANSHELL_QS_PILLS_H

#include "popup.h"
#include "procutil.h"
#include "widgets.h"

#include <QPointer>
#include <QScreen>
#include <QHash>

/* A read-only bar pill fed by a script that prints `icon=`, `text=`, and
 * optional `color=` lines. Polled on `interval`. Hides when it prints
 * nothing. */
class ScriptPill : public BarPill {
    Q_OBJECT
public:
    explicit ScriptPill(const QStringList &command, int intervalMs,
                        QWidget *parent = nullptr);

    /* QML `ScriptPill { tint: ... }` overrides the color= line entirely. */
    void setFixedTint(const QColor &c);

private:
    void parse(const QString &out);

    CollectorProcess m_proc;
    QColor m_fixedTint;
    bool m_hasFixedTint = false;
};

/* The layout of this panel's monitor ("tiling", "monocle", ...). Clicking
 * cycles to the next layout on that monitor, where the backend supports it.
 * Hidden where the window manager reports none (Hyprland, whose slot is the
 * submap's). */
class LayoutWidget : public BarPill {
    Q_OBJECT
public:
    explicit LayoutWidget(QScreen *screen, QWidget *parent = nullptr);

private:
    void sync();

    QPointer<QScreen> m_screen;
};

/* The active keybind submap, where the window manager has them (Hyprland).
 * Shows "global" in the default one, accented while a submap is active;
 * clicking leaves it. */
class SubmapWidget : public BarPill {
    Q_OBJECT
public:
    explicit SubmapWidget(QWidget *parent = nullptr);

private:
    void sync();
};

/* Sequential WireGuard tunnel state (fanshell-qs-vpn). vpn.tunnels in
 * shell.conf exposes tun1..tunN (default one). No polling: status is read once at startup
 * and refreshed after a selection. */
class VpnState : public QObject {
    Q_OBJECT
public:
    static VpnState *instance();

    void refresh();
    void toggle();
    void select(const QString &tunnel);
    bool isUp(const QString &tunnel) const;
    QString address(const QString &tunnel) const;
    QString activeTunnel() const;
    const QStringList &tunnels() const { return m_tunnels; }

signals:
    void changed();

private:
    explicit VpnState(QObject *parent = nullptr);
    void parse(const QString &text, bool fromToggle);

    CollectorProcess m_statusProc;
    CollectorProcess m_toggleProc;
    QStringList m_tunnels;
    QHash<QString, bool> m_up;
    QHash<QString, QString> m_ips;
};

class VpnPopup;

/* WireGuard indicator/selector pill. Click = tunnel menu, right-click =
 * refresh. */
class VpnWidget : public BarPill {
    Q_OBJECT
public:
    explicit VpnWidget(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void sync();

    VpnPopup *m_popup = nullptr;
};

class VpnPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit VpnPopup(QWidget *anchor);

private:
    void sync();

    QVector<ShellButton *> m_buttons;
};

#endif
