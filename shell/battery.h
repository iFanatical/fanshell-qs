/* Battery level/status/time-remaining, backed by scripts/fanshell-qs-battery.
 * Unlike the old bare ScriptPill, this has a hover tooltip and a click
 * popup showing the same "standard battery stuff" (charging/remaining
 * time) a native OS battery flyout would. Hides itself on a machine with
 * no battery. */
#ifndef FANSHELL_QS_BATTERY_H
#define FANSHELL_QS_BATTERY_H

#include "popup.h"
#include "procutil.h"
#include "widgets.h"

class BatteryState : public QObject {
    Q_OBJECT
public:
    static BatteryState *instance();

    bool present = false;
    int percent = 0;
    QString status;
    int minutes = -1; /* -1 = unknown/not applicable */
    QString icon;
    QColor color;

    /* "Charging", "40m until full", "Fully charged", ... */
    QString summary() const;

signals:
    void changed();

private:
    explicit BatteryState(QObject *parent = nullptr);
    void parse(const QString &text);
    void watchUevents();

    CollectorProcess m_proc;
};

class BatteryPopup;

class BatteryWidget : public BarPill {
    Q_OBJECT
public:
    explicit BatteryWidget(QWidget *parent = nullptr);

private:
    void sync();

    BatteryPopup *m_popup;
};

/* Desktop counterpart to the battery pill: a plug glyph with the current
 * power draw. The two are mutually exclusive -- this one shows exactly when
 * BatteryState reports no battery, so a laptop gets charge and a desktop gets
 * watts, with no per-machine configuration. */
class PowerDrawState : public QObject {
    Q_OBJECT
public:
    static PowerDrawState *instance();

    /* Component watts, or -1 when that source is unavailable. */
    qreal cpu = -1;
    qreal gpu = -1;
    /* RAPL present but root-only: worth saying so, since it is fixable. */
    bool cpuLocked = false;
    bool any() const { return cpu >= 0 || gpu >= 0; }

    /* UPS, when ups.host is set in shell.conf and the card answers. Measured at
     * the outlet, so unlike cpu+gpu it is real wall power -- and it covers
     * everything on the UPS, not just this machine. */
    bool upsPresent = false;
    int upsWatts = 0;
    int upsLoad = 0;   /* percent of rated */
    int upsRated = 0;  /* watts */
    int upsMinutes = -1;
    int upsCharge = -1;
    QString upsState;
    bool onBattery = false;
    qreal upsInput = 0;

signals:
    void changed();

private:
    explicit PowerDrawState(QObject *parent = nullptr);
    void parse(const QString &text);
    void parseUps(const QString &text);

    CollectorProcess m_proc;
    CollectorProcess m_ups;
};

class PowerDrawPopup;

class PowerDrawWidget : public BarPill {
    Q_OBJECT
public:
    explicit PowerDrawWidget(QWidget *parent = nullptr);

private:
    void sync();

    PowerDrawPopup *m_popup;
};

class PowerDrawPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit PowerDrawPopup(QWidget *anchor);

private:
    void sync();

    /* Two alternative bodies: UPS telemetry when a card is reachable, the
     * component estimate otherwise. Exactly one group is ever visible. */
    TextItem *m_upsLoad;
    class BatteryBar *m_upsBar;
    TextItem *m_upsRuntime;
    TextItem *m_upsBattery;
    TextItem *m_upsInput;

    TextItem *m_cpu;
    TextItem *m_gpu;
    /* TextItem is single-line, so the footnote is two fixed lines rather than
     * wrapped prose. */
    TextItem *m_note1;
    TextItem *m_note2;
};

class BatteryPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit BatteryPopup(QWidget *anchor);

private:
    void sync();

    TextItem *m_icon;
    TextItem *m_percentText;
    TextItem *m_statusText;
    class BatteryBar *m_bar;
};

#endif
