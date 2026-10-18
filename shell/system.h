/* The "System" dropdown: CPU/memory/temperature (scripts/fanshell-qs-sysinfo) as
 * three side-by-side tiles, a row of Weather + larger VPN/Bluetooth/Clipboard
 * pills (moved off the bar), a standard D-Bus power-profile selector, the
 * keyboard backlight (on a machine that has one), audio
 * (output/mic/device switch), a button into the Displays popup (displays.h),
 * and power buttons backed by scripts/fanshell-qs-power. */
#ifndef FANSHELL_QS_SYSTEM_H
#define FANSHELL_QS_SYSTEM_H

#include "popup.h"
#include "widgets.h"

class SystemPopup;

class SystemWidget : public BarPill {
    Q_OBJECT
public:
    explicit SystemWidget(QWidget *parent = nullptr);

private:
    SystemPopup *m_popup;
};

class SystemPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit SystemPopup(QWidget *anchor);

private:
    void syncAudio();
    void syncKeyboard();
    void rebuildDevices();
    void parseSysInfo(const QString &text);

    class StatBox *m_cpuBox;
    class StatBox *m_memBox;
    class StatBox *m_tempBox;

    class ClickableText *m_outIcon;
    ValueSlider *m_outSlider;
    TextItem *m_outPct;
    class ClickableText *m_micIcon;
    ValueSlider *m_micSlider;
    TextItem *m_micPct;
    class ClickableText *m_kbdIcon = nullptr;
    ValueSlider *m_kbdSlider = nullptr;
    TextItem *m_kbdPct = nullptr;
    /* What the icon restores after switching the backlight off. */
    int m_kbdLastOn = 0;
    QWidget *m_deviceBox;
    QStringList m_deviceKey;
};

#endif
