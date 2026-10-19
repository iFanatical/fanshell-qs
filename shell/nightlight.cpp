#include "nightlight.h"

#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QScreen>
#include <QVBoxLayout>

#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "overlay.h"
#include "paths.h"
#include "theme.h"
#include "widgets.h"

namespace {

constexpr auto confName = "nightlight.conf";
/* The sun's height, in degrees, that is still full day / already night. */
constexpr double dayElevation = 3.0;
constexpr double nightElevation = -6.0;
/* Sunrise and sunset proper: the top of the disc on the horizon. */
constexpr double horizon = -0.833;
constexpr int fadeMs = 1500;

const QVector<int> nightChoices{2500, 3000, 3500, 4000, 4500, 5000, 5500, 6000};
const QVector<int> dayChoices{5500, 6000, 6500};

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

double radians(double d)
{
    return d * M_PI / 180.0;
}

/* The sun's elevation above the horizon in degrees: the low-precision
 * formulas of the Astronomical Almanac, good to a fraction of a degree. */
double solarElevation(const QDateTime &when, double latitude, double longitude)
{
    const double n = when.toMSecsSinceEpoch() / 86400000.0 + 2440587.5 - 2451545.0;
    const double meanLong = std::fmod(280.460 + 0.9856474 * n, 360.0);
    const double anomaly = radians(std::fmod(357.528 + 0.9856003 * n, 360.0));
    const double eclLong = radians(meanLong + 1.915 * std::sin(anomaly)
                                   + 0.020 * std::sin(2 * anomaly));
    const double obliquity = radians(23.439 - 0.0000004 * n);
    const double ra = std::atan2(std::cos(obliquity) * std::sin(eclLong),
                                 std::cos(eclLong));
    const double dec = std::asin(std::sin(obliquity) * std::sin(eclLong));
    const double gmst = std::fmod(18.697374558 + 24.06570982441908 * n, 24.0);
    const double hourAngle = radians(gmst * 15.0 + longitude) - ra;
    const double lat = radians(latitude);
    return std::asin(std::sin(lat) * std::sin(dec)
                     + std::cos(lat) * std::cos(dec) * std::cos(hourAngle))
           * 180.0 / M_PI;
}

struct Rgb {
    double r, g, b;
};

/* A black body's colour (Tanner Helland's fit to the CIE data), 0..255. */
Rgb blackBody(double kelvin)
{
    const double t = kelvin / 100.0;
    auto clamp = [](double v) { return std::clamp(v, 0.0, 255.0); };
    Rgb c;
    c.r = t <= 66 ? 255 : clamp(329.698727446 * std::pow(t - 60, -0.1332047592));
    c.g = t <= 66 ? clamp(99.4708025861 * std::log(t) - 161.1195681661)
                  : clamp(288.1221695283 * std::pow(t - 60, -0.0755148492));
    c.b = t >= 66 ? 255
          : t <= 19 ? 0
                    : clamp(138.5177312231 * std::log(t - 10) - 305.0447927307);
    return c;
}

/* Channel gains for `kelvin`, relative to 6500 K so that it is neutral. */
Rgb gains(int kelvin)
{
    const Rgb c = blackBody(kelvin), white = blackBody(NightLight::neutral);
    return {std::min(1.0, c.r / white.r), std::min(1.0, c.g / white.g),
            std::min(1.0, c.b / white.b)};
}

TextItem *fieldLabel(const QString &text, QWidget *parent)
{
    auto *t = new TextItem(parent);
    t->setPixelSize(Theme::smallFontSize);
    t->setColor(Theme::textMuted);
    t->setText(text);
    return t;
}

QString kelvinLabel(int k)
{
    return k >= NightLight::neutral ? QStringLiteral("%1 K (normal)").arg(k)
                                    : QStringLiteral("%1 K").arg(k);
}

} /* namespace */

/* ---------------------------------------------------------------- popup */

class NightLightWindow : public CenteredOverlay {
public:
    NightLightWindow()
        : CenteredOverlay(QStringLiteral("fanshell-night-light"),
                          QStringLiteral("fanshell-qs-night-light"), 460)
    {
        auto *lay = new QVBoxLayout(box());
        lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                Theme::popupMargin, Theme::popupMargin);
        lay->setSpacing(Theme::popupSpacing);

        auto *title = new TextItem(box());
        title->setPixelSize(Theme::bodyFontSize);
        title->setBold(true);
        title->setColor(Theme::textStrong);
        title->setText(QStringLiteral("Night light"));
        lay->addWidget(title);

        auto *grid = new QGridLayout();
        grid->setHorizontalSpacing(Theme::sectionSpacing);
        grid->setVerticalSpacing(Theme::rowSpacing);
        grid->setColumnStretch(1, 1);

        m_enabled = new ShellButton(QStringLiteral("Off"), box());
        auto *enabledRow = new QWidget(box());
        auto *er = new QHBoxLayout(enabledRow);
        er->setContentsMargins(0, 0, 0, 0);
        er->addWidget(m_enabled);
        er->addStretch(1);
        m_night = new Selector(choiceList(), box());
        m_day = new Selector(choiceList(), box());

        m_location = new TextItem(box());
        m_location->setPixelSize(Theme::smallFontSize);
        m_location->setElide(true);
        m_detect = new ShellButton(QStringLiteral("Detect"), box());
        auto *locationRow = new QWidget(box());
        auto *lr = new QHBoxLayout(locationRow);
        lr->setContentsMargins(0, 0, 0, 0);
        lr->setSpacing(Theme::rowSpacing);
        lr->addWidget(m_location, 1);
        lr->addWidget(m_detect);

        grid->addWidget(fieldLabel(QStringLiteral("Night light"), box()), 0, 0);
        grid->addWidget(enabledRow, 0, 1);
        grid->addWidget(fieldLabel(QStringLiteral("At night"), box()), 1, 0);
        grid->addWidget(m_night, 1, 1);
        grid->addWidget(fieldLabel(QStringLiteral("By day"), box()), 2, 0);
        grid->addWidget(m_day, 2, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Location"), box()), 3, 0);
        grid->addWidget(locationRow, 3, 1);
        lay->addLayout(grid);

        m_now = new TextItem(box());
        m_now->setPixelSize(Theme::tinyFontSize);
        m_now->setColor(Theme::accent);
        lay->addWidget(m_now);

        auto *note = new QWidget(box());
        auto *nl = new QVBoxLayout(note);
        nl->setContentsMargins(0, 0, 0, 0);
        nl->setSpacing(2);
        for (const char *line : {"Warms the screen from sunset, over about 40 minutes,",
                                 "and back by sunrise. The location is looked up from",
                                 "your IP address (wttr.in), as the weather is."}) {
            auto *t = new TextItem(note);
            t->setPixelSize(Theme::tinyFontSize);
            t->setColor(Theme::textMuted);
            t->setText(QString::fromUtf8(line));
            nl->addWidget(t);
        }
        lay->addWidget(note);

        lay->addWidget(new HLine(box()));
        auto *footer = new QWidget(box());
        auto *fl = new QHBoxLayout(footer);
        fl->setContentsMargins(0, 0, 0, 0);
        auto *close = new ShellButton(QStringLiteral("Close"), footer);
        fl->addStretch(1);
        fl->addWidget(close);
        lay->addWidget(footer);

        NightLight *light = NightLight::instance();
        connect(m_enabled, &ShellButton::activated, this, [light]() {
            light->setEnabled(!light->settings().enabled);
        });
        m_night->chosen = [light](int i) {
            NightLight::Settings s = light->settings();
            s.night = nightChoices.value(i, s.night);
            light->set(s);
        };
        m_day->chosen = [light](int i) {
            NightLight::Settings s = light->settings();
            s.day = dayChoices.value(i, s.day);
            light->set(s);
        };
        connect(m_detect, &ShellButton::activated, this, [light]() {
            NightLight::Settings s = light->settings();
            s.autoLocation = true;
            light->set(s);
            light->detectLocation();
        });
        connect(light, &NightLight::changed, this, [this]() {
            if (isVisible())
                sync();
        });
        connect(close, &ShellButton::activated, this, [this]() { dismiss(); });
    }

    void open()
    {
        sync();
        present();
    }

private:
    void sync()
    {
        NightLight *light = NightLight::instance();
        const NightLight::Settings s = light->settings();
        m_enabled->setLabel(s.enabled ? QStringLiteral("On") : QStringLiteral("Off"));
        m_enabled->setActive(s.enabled);

        auto fill = [](Selector *sel, QVector<int> choices, int value) {
            if (!choices.contains(value)) {
                choices << value;
                std::sort(choices.begin(), choices.end());
            }
            QStringList labels;
            for (int k : choices)
                labels << kelvinLabel(k);
            sel->setChoices(labels, int(choices.indexOf(value)));
        };
        fill(m_night, nightChoices, s.night);
        fill(m_day, dayChoices, s.day);
        m_night->setEnabled(s.enabled);
        m_day->setEnabled(s.enabled);

        QString where;
        if (light->detecting())
            where = QStringLiteral("Looking up…");
        else if (s.hasLocation)
            where = QStringLiteral("%1, %2%3")
                        .arg(s.latitude, 0, 'f', 2)
                        .arg(s.longitude, 0, 'f', 2)
                        .arg(s.autoLocation ? QStringLiteral(" (from your IP)")
                                            : QStringLiteral(" (set by hand)"));
        else
            where = QStringLiteral("Unknown; using fixed times");
        m_location->setText(where);
        m_location->setColor(s.hasLocation ? Theme::text : Theme::textMuted);
        m_detect->setEnabled(!light->detecting());

        m_now->setText(s.enabled ? QStringLiteral("Now %1 K · %2")
                                       .arg(light->target())
                                       .arg(light->nextChange())
                                 : QString());
        m_now->setVisible(!m_now->text().isEmpty());
        if (isVisible())
            layoutBox();
    }

    ShellButton *m_enabled;
    Selector *m_night;
    Selector *m_day;
    TextItem *m_location;
    ShellButton *m_detect;
    TextItem *m_now;
};

/* ----------------------------------------------------------- NightLight */

NightLight *NightLight::instance()
{
    static NightLight *s = new NightLight();
    return s;
}

NightLight::NightLight(QObject *parent) : QObject(parent)
{
    const QHash<QString, QString> conf = Paths::readConf(QLatin1String(confName));
    auto on = [&conf](const char *key, bool fallback) {
        const QString v = conf.value(QLatin1String(key));
        return v.isEmpty() ? fallback : v == QLatin1String("on");
    };
    m_settings.enabled = on("enabled", m_settings.enabled);
    m_settings.night = std::clamp(conf.value(QStringLiteral("night"),
                                             QString::number(m_settings.night)).toInt(),
                                  1000, neutral);
    m_settings.day = std::clamp(conf.value(QStringLiteral("day"),
                                           QString::number(m_settings.day)).toInt(),
                                1000, neutral);
    m_settings.autoLocation = conf.value(QStringLiteral("location")) != QLatin1String("manual");
    bool latOk = false, lonOk = false;
    const double lat = conf.value(QStringLiteral("latitude")).toDouble(&latOk);
    const double lon = conf.value(QStringLiteral("longitude")).toDouble(&lonOk);
    if (latOk && lonOk && std::fabs(lat) <= 90 && std::fabs(lon) <= 180) {
        m_settings.hasLocation = true;
        m_settings.latitude = lat;
        m_settings.longitude = lon;
    }

    m_tick.setInterval(60000);
    connect(&m_tick, &QTimer::timeout, this, [this]() {
        retarget(false);
        /* Not found at login (offline, say): try again now and then. */
        if (m_settings.enabled && m_settings.autoLocation && !m_detected
            && !m_detecting && QTime::currentTime().minute() % 15 == 0)
            detectLocation();
    });
    m_tick.start();

    m_fade.setInterval(30);
    connect(&m_fade, &QTimer::timeout, this, [this]() {
        const double t = std::min(1.0, m_fadeClock.elapsed() / double(fadeMs));
        m_current = m_fadeFrom + (m_fadeTo - m_fadeFrom) * t;
        apply(int(std::lround(m_current)));
        if (t >= 1.0)
            m_fade.stop();
    });

    m_hotplug.setSingleShot(true);
    m_hotplug.setInterval(750);
    connect(&m_hotplug, &QTimer::timeout, this, &NightLight::refresh);
    auto hotplug = [this]() { m_hotplug.start(); };
    connect(qApp, &QGuiApplication::screenAdded, this, hotplug);
    connect(qApp, &QGuiApplication::screenRemoved, this, hotplug);

    /* Normal colours again for whatever runs after the shell. */
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_fade.stop();
        apply(neutral);
    });

    retarget(true);
    if (m_settings.enabled && m_settings.autoLocation)
        detectLocation();
}

double NightLight::dayFraction(const QDateTime &when) const
{
    if (m_settings.hasLocation) {
        const double e = solarElevation(when, m_settings.latitude, m_settings.longitude);
        return std::clamp((e - nightElevation) / (dayElevation - nightElevation), 0.0, 1.0);
    }
    const QTime t = when.time();
    const double m = t.hour() * 60 + t.minute() + t.second() / 60.0;
    if (m >= 19.5 * 60 && m < 20.5 * 60)
        return 1.0 - (m - 19.5 * 60) / 60.0;
    if (m >= 6.5 * 60 && m < 7.5 * 60)
        return (m - 6.5 * 60) / 60.0;
    return m >= 7.5 * 60 && m < 19.5 * 60 ? 1.0 : 0.0;
}

int NightLight::target() const
{
    if (!m_settings.enabled)
        return neutral;
    const double f = dayFraction(QDateTime::currentDateTimeUtc());
    return int(std::lround(m_settings.night + (m_settings.day - m_settings.night) * f));
}

QString NightLight::nextChange() const
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!m_settings.hasLocation) {
        const double f = dayFraction(now.toLocalTime());
        return f >= 0.5 ? QStringLiteral("warmer from 19:30")
                        : QStringLiteral("normal by 07:30");
    }
    /* Step ahead until the sun crosses the horizon, then refine. */
    auto above = [this](const QDateTime &t) {
        return solarElevation(t, m_settings.latitude, m_settings.longitude) > horizon;
    };
    const bool upNow = above(now);
    QDateTime lo = now;
    for (int step = 1; step <= 36 * 12; step++) {
        const QDateTime hi = now.addSecs(step * 300);
        if (above(hi) == upNow) {
            lo = hi;
            continue;
        }
        QDateTime a = lo, b = hi;
        while (a.secsTo(b) > 30) {
            const QDateTime mid = a.addSecs(a.secsTo(b) / 2);
            (above(mid) == upNow ? a : b) = mid;
        }
        return (upNow ? QStringLiteral("sunset ") : QStringLiteral("sunrise "))
               + b.toLocalTime().toString(QStringLiteral("HH:mm"));
    }
    return upNow ? QStringLiteral("the sun stays up") : QStringLiteral("the sun stays down");
}

void NightLight::set(const Settings &s)
{
    const bool locate = s.enabled && s.autoLocation && !m_settings.enabled && !m_detected;
    m_settings = s;
    save();
    retarget(true);
    if (locate)
        detectLocation();
    emit changed();
}

void NightLight::setEnabled(bool on)
{
    Settings s = m_settings;
    s.enabled = on;
    set(s);
}

void NightLight::save()
{
    const Settings &s = m_settings;
    QStringList lines{QStringLiteral("enabled=%1").arg(s.enabled ? "on" : "off"),
                      QStringLiteral("night=%1").arg(s.night),
                      QStringLiteral("day=%1").arg(s.day),
                      QStringLiteral("location=%1").arg(s.autoLocation ? "auto" : "manual")};
    if (s.hasLocation)
        lines << QStringLiteral("latitude=%1").arg(s.latitude, 0, 'f', 4)
              << QStringLiteral("longitude=%1").arg(s.longitude, 0, 'f', 4);
    if (!Paths::writeConf(
            QLatin1String(confName),
            QStringLiteral("Night light, set from the Night light popup.\n"
                           "night/day: colour temperature in kelvin; 6500 is "
                           "normal.\nlocation=auto looks it up by IP each "
                           "session; manual keeps latitude/longitude."),
            lines))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(confName))));
}

/* Head for the temperature the sun says, fading there if it is a jump. */
void NightLight::retarget(bool fade)
{
    const int goal = target();
    if (fade && std::fabs(goal - m_current) > 50) {
        m_fadeFrom = m_current;
        m_fadeTo = goal;
        m_fadeClock.start();
        m_fade.start();
        return;
    }
    if (m_fade.isActive()) {
        m_fadeTo = goal;
        return;
    }
    m_current = goal;
    apply(goal);
}

void NightLight::refresh()
{
    /* Off: the ramps are normal or someone else's. */
    if (!m_settings.enabled && !m_fade.isActive())
        return;
    /* Forget what was written: the modeset may have put normal ramps back,
     * or left ours. */
    m_written = -1;
    apply(int(std::lround(m_current)));
}

void NightLight::apply(int kelvin)
{
    /* Normal and already written: leave the ramps to anyone else. */
    if (kelvin >= neutral && m_written >= neutral)
        return;
    xcb_connection_t *c = conn();
    if (!c)
        return;
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &xcb_randr_id);
    if (!ext || !ext->present)
        return;
    const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(
            c, xcb_randr_get_screen_resources_current(c, root), nullptr);
    if (!res)
        return;
    const Rgb g = gains(std::min(kelvin, int(neutral)));
    const xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
    const int count = xcb_randr_get_screen_resources_current_crtcs_length(res);
    for (int i = 0; i < count; i++) {
        xcb_randr_get_crtc_gamma_size_reply_t *sz = xcb_randr_get_crtc_gamma_size_reply(
            c, xcb_randr_get_crtc_gamma_size(c, crtcs[i]), nullptr);
        const int n = sz ? sz->size : 0;
        free(sz);
        if (n < 2)
            continue;
        QVector<uint16_t> ramp(3 * n);
        for (int j = 0; j < n; j++) {
            const double v = double(j) / (n - 1) * 65535.0;
            ramp[j] = uint16_t(std::lround(v * g.r));
            ramp[n + j] = uint16_t(std::lround(v * g.g));
            ramp[2 * n + j] = uint16_t(std::lround(v * g.b));
        }
        /* Unchanged ramps are not written again, so a RandR event caused by
         * writing them cannot lead to another write. */
        bool same = false;
        if (xcb_randr_get_crtc_gamma_reply_t *now = xcb_randr_get_crtc_gamma_reply(
                c, xcb_randr_get_crtc_gamma(c, crtcs[i]), nullptr)) {
            same = now->size == n
                   && std::equal(ramp.constBegin(), ramp.constBegin() + n,
                                 xcb_randr_get_crtc_gamma_red(now))
                   && std::equal(ramp.constBegin() + n, ramp.constBegin() + 2 * n,
                                 xcb_randr_get_crtc_gamma_green(now))
                   && std::equal(ramp.constBegin() + 2 * n, ramp.constEnd(),
                                 xcb_randr_get_crtc_gamma_blue(now));
            free(now);
        }
        if (!same)
            xcb_randr_set_crtc_gamma(c, crtcs[i], uint16_t(n), ramp.constData(),
                                     ramp.constData() + n, ramp.constData() + 2 * n);
    }
    free(res);
    xcb_flush(c);
    m_written = kelvin;
}

void NightLight::detectLocation()
{
    if (m_detecting)
        return;
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    m_detecting = true;
    emit changed();
    QNetworkRequest req(QUrl(QStringLiteral("https://wttr.in/?format=j1")));
    req.setTransferTimeout(10000);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_detecting = false;
        const QJsonObject area = QJsonDocument::fromJson(reply->readAll())
                                     .object()
                                     .value(QStringLiteral("nearest_area"))
                                     .toArray()
                                     .first()
                                     .toObject();
        bool latOk = false, lonOk = false;
        const double lat = area.value(QStringLiteral("latitude")).toString().toDouble(&latOk);
        const double lon = area.value(QStringLiteral("longitude")).toString().toDouble(&lonOk);
        if (reply->error() != QNetworkReply::NoError || !latOk || !lonOk
            || std::fabs(lat) > 90 || std::fabs(lon) > 180) {
            qWarning("fanshell-qs: night light cannot find this machine's location (%s)",
                     qPrintable(reply->errorString()));
            emit changed();
            return;
        }
        m_detected = true;
        /* A manual location set meanwhile wins. */
        if (!m_settings.autoLocation) {
            emit changed();
            return;
        }
        Settings s = m_settings;
        s.hasLocation = true;
        s.latitude = lat;
        s.longitude = lon;
        set(s);
    });
}

void NightLight::openWindow()
{
    if (!m_win)
        m_win = new NightLightWindow();
    m_win->open();
}

void NightLight::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void NightLight::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}
