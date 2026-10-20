/* Night light: warmer colours after sunset, as redshift or f.lux do, in a
 * popup of its own (Control Panel > Night light):
 *
 *     fanshell-qs-shell ipc call nightlight toggle|show|hide
 *     fanshell-qs-shell ipc call nightlight enable|disable|toggle-enabled
 *
 * It follows the sun at the machine's location. Like redshift, it is fully
 * day while the sun is 3 degrees or more above the horizon and fully night
 * once it is 6 degrees below, and moves between the two with the sun's
 * height -- some 40 minutes at mid latitudes, in steps of 150 K. Without a location yet it uses
 * 19:30-20:30 and 06:30-07:30 instead.
 *
 * The location comes from wttr.in by IP, as the weather does, once per
 * session; the last one found is kept for when the network is not up.
 * location=manual in nightlight.conf, with latitude= and longitude=, fixes
 * it instead.
 *
 * The colour is the outputs' RandR gamma ramps, so it composes with digital
 * vibrance (the CTM, applied before them) and works on drivers without a
 * CTM, NVIDIA's included. A modeset can reset the ramps; they are written
 * again after RandR changes, and set back to normal when the shell quits.
 * It fights redshift or gammastep if one runs too. Settings are saved to
 * nightlight.conf in Paths::configDir(). */
#ifndef FANSHELL_QS_NIGHTLIGHT_H
#define FANSHELL_QS_NIGHTLIGHT_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class NightLightWindow;
class QNetworkAccessManager;

class NightLight : public QObject {
    Q_OBJECT
public:
    /* Kelvin; 6500 leaves colours as they are. */
    static constexpr int neutral = 6500;

    struct Settings {
        bool enabled = true;
        int night = 4500;
        int day = neutral;
        bool autoLocation = true;
        bool hasLocation = false;
        double latitude = 0;
        double longitude = 0;
    };

    static NightLight *instance();

    Settings settings() const { return m_settings; }
    /* Apply and save. */
    void set(const Settings &s);
    void setEnabled(bool on);

    /* The colour temperature on screen now, and the one it is heading for. */
    int current() const { return int(m_current); }
    int target() const;
    /* "Sunset 18:31", "Sunrise 07:12", or the fixed times without a
     * location. */
    QString nextChange() const;

    /* Ask wttr.in where this machine is. */
    void detectLocation();
    bool detecting() const { return m_detecting; }

    /* Write the ramps again, after a modeset may have reset them. */
    void refresh();

    void openWindow();
    void toggleWindow();
    void hideWindow();

signals:
    void changed();

private:
    explicit NightLight(QObject *parent = nullptr);
    /* 1 for full day, 0 for full night. */
    double dayFraction(const QDateTime &when) const;
    void retarget(bool fade);
    void apply(int kelvin);
    void save();

    Settings m_settings;
    double m_current = neutral;
    /* What the ramps hold, so normal colours are written once and then
     * left alone for anything else to set. Unknown at first: a shell that
     * crashed at night may have left them warm. */
    int m_written = -1;
    QTimer m_tick;
    QTimer m_fade;
    QTimer m_hotplug;
    QElapsedTimer m_fadeClock;
    double m_fadeFrom = neutral;
    double m_fadeTo = neutral;
    QNetworkAccessManager *m_nam = nullptr;
    bool m_detecting = false;
    bool m_detected = false;
    NightLightWindow *m_win = nullptr;
};

#endif
