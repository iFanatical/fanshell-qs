#include "vibrance.h"

#include <QGuiApplication>
#include <QScreen>

#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <cmath>
#include <cstdlib>

#include "paths.h"
#include "surface.h"
#ifdef FANSHELL_QS_HAVE_WAYLAND
#include "screenshader.h"
#endif

namespace {

constexpr auto confName = "vibrance.conf";
constexpr int ctmItems = 18; /* 9 coefficients x 2 words */

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

xcb_atom_t ctmAtom(xcb_connection_t *c)
{
    static xcb_atom_t atom = XCB_NONE;
    if (atom != XCB_NONE)
        return atom;
    /* only_if_exists: no driver has CTM unless the atom is already there. */
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 1, 3, "CTM"), nullptr);
    if (reply)
        atom = reply->atom;
    free(reply);
    return atom;
}

double clampSaturation(double s)
{
    return qBound(Vibrance::minimum, s, Vibrance::maximum);
}

/* Two decimals is finer than the slider can place, and keeps the file and
 * comparisons free of float noise. */
double rounded(double s)
{
    return std::round(s * 100.0) / 100.0;
}

/* qFuzzyCompare is useless at 0.0, which greyscale legitimately is. */
bool same(double a, double b)
{
    return std::fabs(a - b) < 0.005;
}

/* Saturation the output's CTM currently encodes, or -1 when it has none. */
double readSaturation(xcb_connection_t *c, xcb_randr_output_t output)
{
    const xcb_atom_t atom = ctmAtom(c);
    if (atom == XCB_NONE)
        return -1;
    xcb_randr_get_output_property_reply_t *reply =
        xcb_randr_get_output_property_reply(
            c,
            xcb_randr_get_output_property(c, output, atom, XCB_ATOM_INTEGER,
                                          0, ctmItems, 0, 0),
            nullptr);
    double result = -1;
    if (reply && reply->type == XCB_ATOM_INTEGER && reply->format == 32
            && reply->num_items == ctmItems) {
        const auto *words = reinterpret_cast<const quint32 *>(
            xcb_randr_get_output_property_data(reply));
        double coeff[9];
        for (int i = 0; i < 9; i++) {
            const quint32 low = words[2 * i];
            const quint32 high = words[2 * i + 1];
            const quint64 magnitude =
                (quint64(high & 0x7fffffffu) << 32) | low;
            const double value = double(magnitude) / 4294967296.0;
            coeff[i] = (high & 0x80000000u) ? -value : value;
        }
        /* Diagonal minus off-diagonal recovers s; see vibrance.h. */
        result = rounded(coeff[0] - coeff[1]);
    }
    free(reply);
    return result;
}

void writeSaturation(xcb_connection_t *c, xcb_randr_output_t output,
                     double saturation)
{
    const xcb_atom_t atom = ctmAtom(c);
    if (atom == XCB_NONE)
        return;
    const double off = (1.0 - saturation) / 3.0;
    quint32 words[ctmItems];
    for (int i = 0; i < 9; i++) {
        const double coeff = off + (i % 4 == 0 ? saturation : 0.0);
        quint64 fixed = quint64(std::fabs(coeff) * 4294967296.0);
        if (coeff < 0)
            fixed |= quint64(1) << 63;
        words[2 * i] = quint32(fixed & 0xffffffffu);
        words[2 * i + 1] = quint32(fixed >> 32);
    }
    xcb_randr_change_output_property(c, output, atom, XCB_ATOM_INTEGER, 32,
                                     XCB_PROP_MODE_REPLACE, ctmItems, words);
    xcb_flush(c);
}

} /* namespace */

Vibrance *Vibrance::instance()
{
    static Vibrance *s = new Vibrance();
    return s;
}

Vibrance::Vibrance(QObject *parent) : QObject(parent)
{
    const QHash<QString, QString> conf = Paths::readConf(QLatin1String(confName));
    for (auto it = conf.constBegin(); it != conf.constEnd(); ++it) {
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if (ok)
            m_saved.insert(it.key(), rounded(clampSaturation(value)));
    }

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(400);
    connect(&m_saveTimer, &QTimer::timeout, this, &Vibrance::save);

    /* An output's CTM can be reset by the modeset that brings it up, and Qt
     * reports the screen before that settles, so restore a moment later. */
    m_hotplugTimer.setSingleShot(true);
    m_hotplugTimer.setInterval(750);
    connect(&m_hotplugTimer, &QTimer::timeout, this, [this]() {
        rescan();
        restore();
    });
    auto hotplug = [this]() { m_hotplugTimer.start(); };
    connect(qApp, &QGuiApplication::screenAdded, this, hotplug);
    connect(qApp, &QGuiApplication::screenRemoved, this, hotplug);

    rescan();
    restore();
}

void Vibrance::rescan()
{
    QVector<Output> found;
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* Hyprland: every output, through the screen shader. */
    if (Surface::wayland()) {
        for (const QScreen *s : QGuiApplication::screens()) {
            Output o;
            o.name = s->name();
            o.saturation = normal;
            /* The shader holds what restore() last set. */
            for (const Output &was : std::as_const(m_outputs))
                if (was.name == o.name)
                    o.saturation = was.saturation;
            found.push_back(o);
        }
        m_outputs = found;
        emit changed();
        return;
    }
#endif
    xcb_connection_t *c = conn();
    if (c) {
        const xcb_screen_t *screen =
            xcb_setup_roots_iterator(xcb_get_setup(c)).data;
        xcb_randr_get_screen_resources_current_reply_t *res =
            xcb_randr_get_screen_resources_current_reply(
                c, xcb_randr_get_screen_resources_current(c, screen->root),
                nullptr);
        if (res) {
            const xcb_randr_output_t *ids =
                xcb_randr_get_screen_resources_current_outputs(res);
            const int count =
                xcb_randr_get_screen_resources_current_outputs_length(res);
            for (int i = 0; i < count; i++) {
                xcb_randr_get_output_info_reply_t *info =
                    xcb_randr_get_output_info_reply(
                        c,
                        xcb_randr_get_output_info(c, ids[i],
                                                  res->config_timestamp),
                        nullptr);
                if (info && info->connection == XCB_RANDR_CONNECTION_CONNECTED
                        && info->crtc != XCB_NONE) {
                    const double s = readSaturation(c, ids[i]);
                    if (s >= 0) {
                        Output o;
                        o.id = ids[i];
                        o.name = QString::fromUtf8(
                            reinterpret_cast<const char *>(
                                xcb_randr_get_output_info_name(info)),
                            xcb_randr_get_output_info_name_length(info));
                        o.saturation = s;
                        found.push_back(o);
                    }
                }
                free(info);
            }
            free(res);
        }
    }
    m_outputs = found;
    emit changed();
}

/* Push every saved value back onto its output, if the hardware has drifted
 * from it (a fresh X server starts at the identity matrix). */
void Vibrance::restore()
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    if (Surface::wayland()) {
        for (Output &o : m_outputs) {
            o.saturation = m_saved.value(o.name, normal);
            ScreenShader::instance()->setSaturation(o.name, o.saturation);
        }
        emit changed();
        return;
    }
#endif
    xcb_connection_t *c = conn();
    if (!c)
        return;
    bool any = false;
    for (Output &o : m_outputs) {
        const auto it = m_saved.constFind(o.name);
        if (it == m_saved.constEnd() || same(*it, o.saturation))
            continue;
        writeSaturation(c, o.id, *it);
        o.saturation = *it;
        any = true;
    }
    if (any)
        emit changed();
}

void Vibrance::refresh()
{
    rescan();
    restore();
}

double Vibrance::saturation(const QString &output) const
{
    for (const Output &o : m_outputs)
        if (o.name == output)
            return o.saturation;
    return -1;
}

void Vibrance::setSaturation(const QString &output, double saturation)
{
    xcb_connection_t *c = conn();
    if (!c && !Surface::wayland())
        return;
    saturation = rounded(clampSaturation(saturation));
    for (Output &o : m_outputs) {
        if (o.name != output)
            continue;
        if (same(o.saturation, saturation))
            return;
#ifdef FANSHELL_QS_HAVE_WAYLAND
        if (Surface::wayland())
            ScreenShader::instance()->setSaturation(o.name, saturation);
        else
#endif
            writeSaturation(c, o.id, saturation);
        o.saturation = saturation;
        m_saved.insert(o.name, saturation);
        m_saveTimer.start();
        emit changed();
        return;
    }
}

void Vibrance::save()
{
    QStringList names = m_saved.keys();
    names.sort();
    QStringList lines;
    for (const QString &name : names)
        lines << name + QLatin1Char('=') + QString::number(m_saved.value(name), 'f', 2);
    if (!Paths::writeConf(QLatin1String(confName),
                          QStringLiteral("Digital vibrance per output, set from "
                                         "the System menu.\n1.00 is normal, "
                                         "0.00 greyscale, up to 2.00."),
                          lines))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(confName))));
}
