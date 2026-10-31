#include "displays.h"

#include <QAbstractNativeEventFilter>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <xcb/dpms.h>
#include <xcb/randr.h>
#include <xcb/screensaver.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <unistd.h>

#include "wmstate.h"
#ifdef FANSHELL_QS_HAVE_WAYLAND
#include "hypripc.h"
#include "wlclient.h"
#endif
#include "idleinhibit.h"
#include "lockscreen.h"
#include "nightlight.h"
#include "overlay.h"
#include "paths.h"
#include "popup.h"
#include "surface.h"
#include "theme.h"
#include "vibrance.h"
#include "volume.h"
#include "widgets.h"

using Displays::Mode;
using Displays::Output;

namespace {

/* How long a trial layout stays before reverting by itself. */
constexpr int confirmSeconds = 15;
/* RandR sends a burst of events per change; act once it has gone quiet. */
constexpr int settleMs = 1000;
constexpr int boxWidth = 640;

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

xcb_window_t rootWindow(xcb_connection_t *c)
{
    return xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
}

/* Refresh as xrandr computes it, to two decimals so `--rate` matches. */
double modeRate(const xcb_randr_mode_info_t &m)
{
    if (!m.htotal || !m.vtotal)
        return 0;
    double lines = m.vtotal;
    if (m.mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
        lines *= 2;
    if (m.mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)
        lines /= 2;
    return std::round(double(m.dot_clock) / (double(m.htotal) * lines) * 100.0)
           / 100.0;
}

QString rotationName(uint16_t rotation)
{
    switch (rotation & 0xf) {
    case XCB_RANDR_ROTATION_ROTATE_90:
        return QStringLiteral("left");
    case XCB_RANDR_ROTATION_ROTATE_180:
        return QStringLiteral("inverted");
    case XCB_RANDR_ROTATION_ROTATE_270:
        return QStringLiteral("right");
    default:
        return QStringLiteral("normal");
    }
}

xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 1, uint16_t(strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

/* An output's TearFree setting, or empty when it has no such property. */
QString tearFree(xcb_connection_t *c, xcb_randr_output_t output, xcb_atom_t prop)
{
    if (prop == XCB_ATOM_NONE)
        return QString();
    QString value;
    xcb_randr_get_output_property_reply_t *r = xcb_randr_get_output_property_reply(
        c, xcb_randr_get_output_property(c, output, prop, XCB_ATOM_ATOM, 0, 1, 0, 0),
        nullptr);
    if (r && r->type == XCB_ATOM_ATOM && r->format == 32 && r->num_items == 1) {
        const xcb_atom_t a =
            *reinterpret_cast<const xcb_atom_t *>(xcb_randr_get_output_property_data(r));
        if (xcb_get_atom_name_reply_t *n = xcb_get_atom_name_reply(
                c, xcb_get_atom_name(c, a), nullptr)) {
            value = QString::fromLatin1(xcb_get_atom_name_name(n),
                                        xcb_get_atom_name_name_length(n));
            free(n);
        }
    }
    free(r);
    return value;
}

bool sameRate(double a, double b)
{
    return std::fabs(a - b) < 0.005;
}

/* Shift the layout so its top-left enabled corner is the origin, as xrandr
 * would otherwise grow the screen to include the empty space. */
void normalize(QVector<Output> &outputs)
{
    int minX = INT_MAX, minY = INT_MAX;
    for (const Output &o : outputs)
        if (o.enabled) {
            minX = qMin(minX, o.x);
            minY = qMin(minY, o.y);
        }
    if (minX == INT_MAX)
        return;
    for (Output &o : outputs)
        if (o.enabled) {
            o.x -= minX;
            o.y -= minY;
        }
}

int enabledCount(const QVector<Output> &outputs)
{
    int n = 0;
    for (const Output &o : outputs)
        n += o.enabled ? 1 : 0;
    return n;
}

#ifdef FANSHELL_QS_HAVE_WAYLAND
/* Hyprland's transform: 0 normal, 1 90 degrees (xrandr's left), 2 180,
 * 3 270 (right); 4-7 are the flipped ones, shown as their unflipped
 * rotation. */
QString transformName(int t)
{
    switch (t & 3) {
    case 1:
        return QStringLiteral("left");
    case 2:
        return QStringLiteral("inverted");
    case 3:
        return QStringLiteral("right");
    default:
        return QStringLiteral("normal");
    }
}

/* "1920x1080@165.00Hz", as availableModes lists them. */
bool parseMode(const QString &text, Mode &m)
{
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)x(\\d+)@([0-9.]+)"));
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch())
        return false;
    m.w = match.captured(1).toInt();
    m.h = match.captured(2).toInt();
    m.rate = std::round(match.captured(3).toDouble() * 100.0) / 100.0;
    return true;
}

QVector<Output> queryHyprland()
{
    QVector<Output> out;
    for (const QJsonValue &v : HyprIpc::requestArray("monitors all")) {
        const QJsonObject j = v.toObject();
        Output o;
        o.name = j.value(QStringLiteral("name")).toString();
        if (o.name.isEmpty())
            continue;
        o.enabled = !j.value(QStringLiteral("disabled")).toBool();
        o.x = j.value(QStringLiteral("x")).toInt();
        o.y = j.value(QStringLiteral("y")).toInt();
        o.w = j.value(QStringLiteral("width")).toInt();
        o.h = j.value(QStringLiteral("height")).toInt();
        o.rate = std::round(j.value(QStringLiteral("refreshRate")).toDouble() * 100.0)
                 / 100.0;
        o.rotate = transformName(j.value(QStringLiteral("transform")).toInt());
        o.scale = j.value(QStringLiteral("scale")).toDouble(1.0);
        if (o.scale <= 0)
            o.scale = 1.0;
        o.vrr = j.value(QStringLiteral("vrr")).toBool() ? QStringLiteral("on")
                                                         : QStringLiteral("off");
        bool first = true;
        for (const QJsonValue &mv : j.value(QStringLiteral("availableModes")).toArray()) {
            Mode m;
            if (!parseMode(mv.toString(), m))
                continue;
            /* The connector's preferred mode comes first. */
            if (first)
                o.preferred = m;
            first = false;
            bool seen = false;
            for (const Mode &q : o.modes)
                seen = seen || (q.w == m.w && q.h == m.h && sameRate(q.rate, m.rate));
            if (!seen)
                o.modes << m;
        }
        std::sort(o.modes.begin(), o.modes.end(), [](const Mode &a, const Mode &b) {
            if (a.w * a.h != b.w * b.h)
                return a.w * a.h > b.w * b.h;
            return a.rate > b.rate;
        });
        if (o.modes.isEmpty() && o.w > 0)
            o.modes << Mode{o.w, o.h, o.rate};
        if (o.preferred.w <= 0 && !o.modes.isEmpty())
            o.preferred = o.modes.first();
        if (!o.enabled || o.w <= 0) {
            o.w = o.preferred.w;
            o.h = o.preferred.h;
            o.rate = o.preferred.rate;
        }
        out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const Output &a, const Output &b) {
        return a.name < b.name;
    });
    return out;
}
#endif

} /* namespace */

/* ------------------------------------------------------------- Displays:: */

QVector<Output> Displays::query()
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    if (Surface::wayland())
        return queryHyprland();
#endif
    QVector<Output> out;
    xcb_connection_t *c = conn();
    if (!c)
        return out;
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_randr_id);
    if (!ext || !ext->present)
        return out;
    const xcb_window_t root = rootWindow(c);
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(
            c, xcb_randr_get_screen_resources_current(c, root), nullptr);
    if (!res)
        return out;

    QHash<quint32, xcb_randr_mode_info_t> modes;
    const xcb_randr_mode_info_t *info =
        xcb_randr_get_screen_resources_current_modes(res);
    for (int i = 0; i < xcb_randr_get_screen_resources_current_modes_length(res);
         i++)
        modes.insert(info[i].id, info[i]);

    xcb_randr_output_t primary = XCB_NONE;
    if (auto *p = xcb_randr_get_output_primary_reply(
            c, xcb_randr_get_output_primary(c, root), nullptr)) {
        primary = p->output;
        free(p);
    }

    const xcb_atom_t tearFreeAtom = atom(c, "TearFree");
    const xcb_randr_output_t *ids =
        xcb_randr_get_screen_resources_current_outputs(res);
    const int count = xcb_randr_get_screen_resources_current_outputs_length(res);
    for (int i = 0; i < count; i++) {
        xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(
            c, xcb_randr_get_output_info(c, ids[i], res->config_timestamp),
            nullptr);
        if (!oi || oi->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oi);
            continue;
        }
        Output o;
        o.name = QString::fromUtf8(
            reinterpret_cast<const char *>(xcb_randr_get_output_info_name(oi)),
            xcb_randr_get_output_info_name_length(oi));

        const xcb_randr_mode_t *mids = xcb_randr_get_output_info_modes(oi);
        const int nmodes = xcb_randr_get_output_info_modes_length(oi);
        for (int j = 0; j < nmodes; j++) {
            const auto it = modes.constFind(mids[j]);
            /* Interlaced modes are named differently (1920x1080i), so
             * `--mode WxH` could not select them; leave them out. */
            if (it == modes.constEnd()
                    || (it->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE))
                continue;
            const Mode m{it->width, it->height, modeRate(*it)};
            if (j < oi->num_preferred && o.preferred.w == 0)
                o.preferred = m;
            const bool dup = std::any_of(
                o.modes.cbegin(), o.modes.cend(), [&m](const Mode &k) {
                    return k.w == m.w && k.h == m.h && sameRate(k.rate, m.rate);
                });
            if (!dup)
                o.modes.push_back(m);
        }
        std::sort(o.modes.begin(), o.modes.end(),
                  [](const Mode &a, const Mode &b) {
                      if (a.w * a.h != b.w * b.h)
                          return a.w * a.h > b.w * b.h;
                      if (a.w != b.w)
                          return a.w > b.w;
                      return a.rate > b.rate;
                  });
        if (o.preferred.w == 0 && !o.modes.isEmpty())
            o.preferred = o.modes.first();

        if (oi->crtc != XCB_NONE) {
            xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(
                c, xcb_randr_get_crtc_info(c, oi->crtc, res->config_timestamp),
                nullptr);
            if (ci && ci->mode != XCB_NONE) {
                o.enabled = true;
                o.x = ci->x;
                o.y = ci->y;
                o.rotate = rotationName(ci->rotation);
                const auto it = modes.constFind(ci->mode);
                if (it != modes.constEnd()) {
                    o.w = it->width;
                    o.h = it->height;
                    o.rate = modeRate(*it);
                }
            }
            free(ci);
        }
        if (!o.enabled) {
            o.w = o.preferred.w;
            o.h = o.preferred.h;
            o.rate = o.preferred.rate;
        }
        o.primary = o.enabled && ids[i] == primary;
        o.tearFree = tearFree(c, ids[i], tearFreeAtom);
        out.push_back(o);
        free(oi);
    }
    free(res);

    std::sort(out.begin(), out.end(), [](const Output &a, const Output &b) {
        return a.name < b.name;
    });
    return out;
}

QString Displays::key(const QVector<Output> &outputs)
{
    QStringList names;
    for (const Output &o : outputs)
        names << o.name;
    names.sort();
    return names.join(QLatin1Char('+'));
}

QString Displays::profile(const QVector<Output> &outputs)
{
    QString s = QLatin1Char('[') + key(outputs) + QStringLiteral("]\n");
    for (const Output &o : outputs) {
        s += o.name;
        if (!o.enabled) {
            s += QStringLiteral(" off\n");
            continue;
        }
        s += QStringLiteral(" mode=%1x%2").arg(o.w).arg(o.h);
        if (o.rate > 0)
            s += QStringLiteral(" rate=%1").arg(o.rate, 0, 'f', 2);
        s += QStringLiteral(" pos=%1x%2 rotate=%3").arg(o.x).arg(o.y).arg(o.rotate);
        if (std::fabs(o.scale - 1.0) > 0.001)
            s += QStringLiteral(" scale=%1").arg(o.scale, 0, 'g', 6);
        if (!o.vrr.isEmpty())
            s += QStringLiteral(" vrr=") + o.vrr;
        if (!o.tearFree.isEmpty())
            s += QStringLiteral(" tearfree=") + o.tearFree;
        if (o.primary)
            s += QStringLiteral(" primary");
        s += QLatin1Char('\n');
    }
    return s;
}

namespace {

/* -------------------------------------------------------- ArrangementView */

/* The outputs to scale, as boxes to drag. A dropped box snaps flush to its
 * neighbours' edges and is pushed out of any overlap, so the usual layouts
 * (side by side, stacked, aligned tops) need no precision. */
class ArrangementView : public QWidget {
public:
    ArrangementView(QVector<Output> *outputs, int *selected, QWidget *parent)
        : QWidget(parent), m_outputs(outputs), m_selected(selected)
    {
        setFixedHeight(220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    std::function<void()> selectionChanged;
    std::function<void()> layoutChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QColor ground = Theme::surface;
        ground.setAlpha(110);
        Theme::paintRect(p, rect(), ground, Theme::radius);
        if (m_drag < 0)
            fit();
        for (int i = 0; i < m_outputs->size(); i++) {
            const Output &o = (*m_outputs)[i];
            if (!o.enabled)
                continue;
            const bool sel = i == *m_selected;
            const QRectF r = toView(o.rect()).adjusted(2, 2, -2, -2);
            Theme::paintRect(p, r, sel ? Theme::surfaceHover : Theme::surface,
                             Theme::radius, sel ? Theme::accent : Theme::border,
                             sel ? 2 : 1);
            p.save();
            p.setClipRect(r.adjusted(4, 4, -4, -4));
            p.setPen(Theme::textStrong);
            p.setFont(Theme::font(Theme::smallFontSize, true));
            /* Stacked lines, each elided to the box, so a small or portrait
             * box stays legible. */
            const int room = qMax(0, int(r.width()) - 8);
            auto line = [&p, &r, room](qreal dy, const QString &text) {
                p.drawText(r.adjusted(0, dy, 0, dy), Qt::AlignCenter,
                           p.fontMetrics().elidedText(text, Qt::ElideRight,
                                                      room));
            };
            const qreal shift = o.primary ? 8 : 0;
            line(-10 - shift, o.name);
            p.setPen(Theme::textMuted);
            p.setFont(Theme::font(Theme::tinyFontSize));
            line(10 - shift,
                 QStringLiteral("%1 × %2").arg(o.width()).arg(o.height()));
            if (o.primary) {
                p.setPen(Theme::accent);
                line(26, QStringLiteral("primary"));
            }
            p.restore();
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        const int i = hit(e->position());
        if (i < 0)
            return;
        *m_selected = i;
        if (selectionChanged)
            selectionChanged();
        m_drag = i;
        m_moved = false;
        m_grab = e->position();
        m_origin = QPoint((*m_outputs)[i].x, (*m_outputs)[i].y);
        update();
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_drag < 0)
            return;
        const QPointF d = (e->position() - m_grab) / m_scale;
        Output &o = (*m_outputs)[m_drag];
        o.x = m_origin.x() + qRound(d.x());
        o.y = m_origin.y() + qRound(d.y());
        m_moved = true;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_drag < 0)
            return;
        const int i = m_drag;
        m_drag = -1;
        if (m_moved) {
            snap(i);
            normalize(*m_outputs);
            if (layoutChanged)
                layoutChanged();
        }
        update();
    }

private:
    /* Scale and centre the enabled outputs' bounds into the view. Held
     * still while dragging, so the box under the pointer does not slide. */
    void fit()
    {
        QRect bounds;
        for (const Output &o : *m_outputs)
            if (o.enabled)
                bounds = bounds.united(o.rect());
        if (bounds.isEmpty())
            bounds = QRect(0, 0, 1920, 1080);
        const qreal margin = 18;
        m_scale = qMin((width() - 2 * margin) / bounds.width(),
                       (height() - 2 * margin) / bounds.height());
        m_offset = QPointF(
            (width() - bounds.width() * m_scale) / 2 - bounds.x() * m_scale,
            (height() - bounds.height() * m_scale) / 2 - bounds.y() * m_scale);
    }

    QRectF toView(const QRect &r) const
    {
        return QRectF(m_offset.x() + r.x() * m_scale,
                      m_offset.y() + r.y() * m_scale, r.width() * m_scale,
                      r.height() * m_scale);
    }

    int hit(const QPointF &pos) const
    {
        for (int i = m_outputs->size() - 1; i >= 0; i--)
            if ((*m_outputs)[i].enabled
                    && toView((*m_outputs)[i].rect()).contains(pos))
                return i;
        return -1;
    }

    void snap(int i)
    {
        Output &o = (*m_outputs)[i];
        const int w = o.width(), h = o.height();
        const int threshold = qRound(20 / m_scale);
        int bestX = o.x, bestY = o.y;
        int dx = threshold + 1, dy = threshold + 1;
        bool others = false;
        for (int j = 0; j < m_outputs->size(); j++) {
            const Output &q = (*m_outputs)[j];
            if (j == i || !q.enabled)
                continue;
            others = true;
            const QRect r = q.rect();
            for (int cx : {r.x() - w, r.x() + r.width(), r.x(),
                           r.x() + r.width() - w})
                if (std::abs(cx - o.x) < dx) {
                    dx = std::abs(cx - o.x);
                    bestX = cx;
                }
            for (int cy : {r.y() - h, r.y() + r.height(), r.y(),
                           r.y() + r.height() - h})
                if (std::abs(cy - o.y) < dy) {
                    dy = std::abs(cy - o.y);
                    bestY = cy;
                }
        }
        if (!others) {
            o.x = o.y = 0;
            return;
        }
        if (dx <= threshold)
            o.x = bestX;
        if (dy <= threshold)
            o.y = bestY;

        /* Outputs may not overlap (that would be mirroring, which this does
         * not offer): move to whichever side of the obstacle is nearest. */
        for (int pass = 0; pass < 8; pass++) {
            const QRect mine = o.rect();
            const Output *block = nullptr;
            for (int j = 0; j < m_outputs->size(); j++)
                if (j != i && (*m_outputs)[j].enabled
                        && (*m_outputs)[j].rect().intersects(mine))
                    block = &(*m_outputs)[j];
            if (!block)
                break;
            const QRect r = block->rect();
            const QPoint options[] = {
                {r.x() - w, o.y}, {r.x() + r.width(), o.y},
                {o.x, r.y() - h}, {o.x, r.y() + r.height()}};
            QPoint best = options[0];
            for (const QPoint &opt : options)
                if ((opt - QPoint(o.x, o.y)).manhattanLength()
                        < (best - QPoint(o.x, o.y)).manhattanLength())
                    best = opt;
            o.x = best.x();
            o.y = best.y();
        }
    }

    QVector<Output> *m_outputs;
    int *m_selected;
    qreal m_scale = 0.1;
    QPointF m_offset;
    int m_drag = -1;
    bool m_moved = false;
    QPointF m_grab;
    QPoint m_origin;
};

TextItem *fieldLabel(const QString &text, QWidget *parent)
{
    auto *t = new TextItem(parent);
    t->setPixelSize(Theme::smallFontSize);
    t->setColor(Theme::textMuted);
    t->setText(text);
    return t;
}

const QStringList rotations{QStringLiteral("normal"), QStringLiteral("left"),
                            QStringLiteral("right"), QStringLiteral("inverted")};
const QStringList rotationLabels{QStringLiteral("Normal"),
                                 QStringLiteral("Left"), QStringLiteral("Right"),
                                 QStringLiteral("Upside down")};
/* Hyprland only. */
const QVector<double> scales{1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
const QStringList vrrModes{QStringLiteral("off"), QStringLiteral("on"),
                           QStringLiteral("fullscreen")};
const QStringList vrrLabels{QStringLiteral("Off"), QStringLiteral("On"),
                            QStringLiteral("Fullscreen only")};

QString scaleLabel(double s)
{
    return QStringLiteral("%1%").arg(qRound(s * 100));
}

const QStringList tearFreeModes{QStringLiteral("auto"), QStringLiteral("on"),
                                QStringLiteral("off")};
const QStringList tearFreeLabels{QStringLiteral("Auto"), QStringLiteral("On"),
                                 QStringLiteral("Off")};

} /* namespace */


/* --------------------------------------------------------- DisplaysWindow */

class DisplaysWindow : public CenteredOverlay {
public:
    DisplaysWindow();

    void openOn(bool reload);
    /* User dismissal: a pending trial is reverted. */
    void dismiss() override;
    bool busy() const { return m_pending || m_running; }
    void outputsSettled();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void load();
    void rebuildChips();
    void syncAll();
    void syncDetails();
    void syncVibrance();
    void syncFooter();
    bool dirty() const;
    Output *selected();
    /* `scale` 0 keeps the output's own. */
    void resizeSelected(int w, int h, const QString &rotate, double scale = 0);
    void setOutputOn(bool on);
    void apply();
    void keep();
    void revert();

    QVector<Output> m_outputs;
    QVector<Output> m_before; /* live state the trial reverts to */
    int m_selected = 0;
    bool m_pending = false;   /* trial applied, awaiting Keep */
    bool m_running = false;   /* script in flight */
    int m_countdown = 0;
    QString m_error;
    QTimer m_tick;

    QWidget *m_box;
    QHBoxLayout *m_chipRow;
    QVector<ShellButton *> m_chips;
    ArrangementView *m_view;
    TextItem *m_heading;
    ShellButton *m_enabled;
    ShellButton *m_primary;
    Selector *m_resolution;
    Selector *m_rate;
    Selector *m_rotation;
    QWidget *m_tearFreeLabel;
    Selector *m_tearFree;
    QWidget *m_scaleLabel;
    Selector *m_scale;
    QWidget *m_vrrLabel;
    Selector *m_vrr;
    QWidget *m_vibranceLabel;
    QWidget *m_vibranceRow;
    ValueSlider *m_vibrance;
    ClickableText *m_vibrancePct;
    TextItem *m_status;
    ShellButton *m_resetButton;
    ShellButton *m_applyButton;
    ShellButton *m_revertButton;
    ShellButton *m_keepButton;
    ChoiceList *m_list;
};

DisplaysWindow::DisplaysWindow()
    : CenteredOverlay(QStringLiteral("fanshell-displays"),
                      QStringLiteral("fanshell-qs-displays"), boxWidth)
{
    m_box = box();
    auto *lay = new QVBoxLayout(m_box);
    lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                            Theme::popupMargin, Theme::popupMargin);
    lay->setSpacing(Theme::popupSpacing);

    auto *title = new TextItem(m_box);
    title->setPixelSize(Theme::bodyFontSize);
    title->setBold(true);
    title->setColor(Theme::textStrong);
    title->setText(QStringLiteral("Displays"));
    lay->addWidget(title);

    /* Every connected output, on or off: the canvas only shows those on. */
    auto *chips = new QWidget(m_box);
    m_chipRow = new QHBoxLayout(chips);
    m_chipRow->setContentsMargins(0, 0, 0, 0);
    m_chipRow->setSpacing(Theme::rowSpacing);
    m_chipRow->addStretch(1);
    lay->addWidget(chips);

    m_view = new ArrangementView(&m_outputs, &m_selected, m_box);
    m_view->selectionChanged = [this]() {
        rebuildChips();
        syncDetails();
    };
    m_view->layoutChanged = [this]() { syncAll(); };
    lay->addWidget(m_view);

    lay->addWidget(new HLine(m_box));

    m_heading = new TextItem(m_box);
    m_heading->setPixelSize(Theme::bodyFontSize);
    m_heading->setBold(true);
    lay->addWidget(m_heading);

    m_list = choiceList();

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(Theme::sectionSpacing);
    grid->setVerticalSpacing(Theme::rowSpacing);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);

    auto *toggles = new QWidget(m_box);
    auto *tl = new QHBoxLayout(toggles);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(Theme::rowSpacing);
    m_enabled = new ShellButton(QStringLiteral("On"), toggles);
    m_primary = new ShellButton(QStringLiteral("Primary"), toggles);
    tl->addWidget(m_enabled);
    tl->addWidget(m_primary);
    tl->addStretch(1);
    /* Hyprland has no primary output. */
    m_primary->setVisible(!Surface::wayland());

    m_resolution = new Selector(m_list, m_box);
    m_rate = new Selector(m_list, m_box);
    m_rotation = new Selector(m_list, m_box);
    m_tearFree = new Selector(m_list, m_box);
    m_tearFree->setToolTip(QStringLiteral(
        "The driver's tear-free page flipping. Auto: only where needed, such "
        "as a rotated output"));

    grid->addWidget(fieldLabel(QStringLiteral("Output"), m_box), 0, 0);
    grid->addWidget(toggles, 0, 1, 1, 3);
    grid->addWidget(fieldLabel(QStringLiteral("Resolution"), m_box), 1, 0);
    grid->addWidget(m_resolution, 1, 1);
    grid->addWidget(fieldLabel(QStringLiteral("Refresh"), m_box), 1, 2);
    grid->addWidget(m_rate, 1, 3);
    grid->addWidget(fieldLabel(QStringLiteral("Rotation"), m_box), 2, 0);
    grid->addWidget(m_rotation, 2, 1);
    m_tearFreeLabel = fieldLabel(QStringLiteral("TearFree"), m_box);
    grid->addWidget(m_tearFreeLabel, 2, 2);
    grid->addWidget(m_tearFree, 2, 3);

    m_scale = new Selector(m_list, m_box);
    m_scale->setToolTip(QStringLiteral(
        "How large everything is drawn on this display"));
    m_vrr = new Selector(m_list, m_box);
    m_vrr->setToolTip(QStringLiteral(
        "Adaptive sync (FreeSync, G-Sync): the display follows the frame "
        "rate. Fullscreen only: just for fullscreen windows, such as games"));
    m_scaleLabel = fieldLabel(QStringLiteral("Scale"), m_box);
    m_vrrLabel = fieldLabel(QStringLiteral("Adaptive sync"), m_box);
    grid->addWidget(m_scaleLabel, 3, 0);
    grid->addWidget(m_scale, 3, 1);
    grid->addWidget(m_vrrLabel, 3, 2);
    grid->addWidget(m_vrr, 3, 3);

    /* Vibrance applies (and saves) at once, outside the layout trial. */
    m_vibranceLabel = fieldLabel(QStringLiteral("Vibrance"), m_box);
    m_vibranceRow = new QWidget(m_box);
    auto *vl = new QHBoxLayout(m_vibranceRow);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(Theme::rowSpacing);
    m_vibrance = new ValueSlider(m_vibranceRow);
    m_vibrance->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_vibrancePct = new ClickableText(m_vibranceRow);
    m_vibrancePct->setPixelSize(Theme::smallFontSize);
    m_vibrancePct->setHAlign(Qt::AlignRight);
    m_vibrancePct->setFixedWidth(44);
    m_vibrancePct->setToolTip(QStringLiteral("Reset to 100%"));
    vl->addWidget(m_vibrance, 1, Qt::AlignVCenter);
    vl->addWidget(m_vibrancePct, 0, Qt::AlignVCenter);
    grid->addWidget(m_vibranceLabel, 4, 0);
    grid->addWidget(m_vibranceRow, 4, 1, 1, 3);
    lay->addLayout(grid);

    lay->addWidget(new HLine(m_box));

    auto *footer = new QWidget(m_box);
    auto *fl = new QHBoxLayout(footer);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(Theme::rowSpacing);
    m_status = new TextItem(footer);
    m_status->setPixelSize(Theme::smallFontSize);
    m_status->setElide(true);
    m_resetButton = new ShellButton(QStringLiteral("Reset"), footer);
    m_applyButton = new ShellButton(QStringLiteral("Apply"), footer);
    m_revertButton = new ShellButton(QStringLiteral("Revert"), footer);
    m_keepButton = new ShellButton(QStringLiteral("Keep"), footer);
    fl->addWidget(m_status, 1, Qt::AlignVCenter);
    fl->addWidget(m_resetButton);
    fl->addWidget(m_applyButton);
    fl->addWidget(m_revertButton);
    fl->addWidget(m_keepButton);
    lay->addWidget(footer);

    connect(m_enabled, &ShellButton::activated, this, [this]() {
        if (Output *o = selected())
            setOutputOn(!o->enabled);
    });
    connect(m_primary, &ShellButton::activated, this, [this]() {
        Output *o = selected();
        if (!o || !o->enabled)
            return;
        const bool on = !o->primary;
        for (Output &q : m_outputs)
            q.primary = false;
        o->primary = on;
        syncAll();
    });
    m_resolution->chosen = [this](int i) {
        Output *o = selected();
        if (!o)
            return;
        /* The list is the output's distinct sizes, in mode order. */
        QVector<QSize> sizes;
        for (const Mode &m : o->modes)
            if (!sizes.contains(QSize(m.w, m.h)))
                sizes << QSize(m.w, m.h);
        if (i < 0 || i >= sizes.size())
            return;
        const QSize size = sizes[i];
        /* Keep the refresh rate if the new size offers it, else the fastest. */
        double rate = -1;
        for (const Mode &m : o->modes)
            if (m.w == size.width() && m.h == size.height()
                    && (rate < 0 || sameRate(m.rate, o->rate)))
                rate = m.rate;
        o->rate = rate;
        resizeSelected(size.width(), size.height(), o->rotate);
    };
    m_rate->chosen = [this](int i) {
        Output *o = selected();
        if (!o)
            return;
        QVector<double> rates;
        for (const Mode &m : o->modes)
            if (m.w == o->w && m.h == o->h)
                rates << m.rate;
        if (i >= 0 && i < rates.size()) {
            o->rate = rates[i];
            syncAll();
        }
    };
    m_rotation->chosen = [this](int i) {
        if (Output *o = selected())
            resizeSelected(o->w, o->h, rotations.value(i, o->rotate));
    };
    m_scale->chosen = [this](int i) {
        Output *o = selected();
        if (!o || i < 0)
            return;
        QVector<double> offered = scales;
        if (!offered.contains(o->scale))
            offered << o->scale;
        std::sort(offered.begin(), offered.end());
        if (i < offered.size())
            resizeSelected(o->w, o->h, o->rotate, offered[i]);
    };
    m_vrr->chosen = [this](int i) {
        Output *o = selected();
        if (o && i >= 0 && i < vrrModes.size()) {
            o->vrr = vrrModes[i];
            syncAll();
        }
    };
    m_tearFree->chosen = [this](int i) {
        Output *o = selected();
        if (o && i >= 0 && i < tearFreeModes.size()) {
            o->tearFree = tearFreeModes[i];
            syncAll();
        }
    };

    connect(m_vibrance, &ValueSlider::moved, this, [this](qreal v) {
        if (Output *o = selected())
            Vibrance::instance()->setSaturation(o->name, v * Vibrance::maximum);
    });
    connect(m_vibrancePct, &ClickableText::clicked, this, [this]() {
        if (Output *o = selected())
            Vibrance::instance()->setSaturation(o->name, Vibrance::normal);
    });
    connect(Vibrance::instance(), &Vibrance::changed, this,
            [this]() { syncVibrance(); });

    connect(m_resetButton, &ShellButton::activated, this, [this]() {
        m_error.clear();
        load();
    });
    connect(m_applyButton, &ShellButton::activated, this, [this]() { apply(); });
    connect(m_keepButton, &ShellButton::activated, this, [this]() { keep(); });
    connect(m_revertButton, &ShellButton::activated, this,
            [this]() { revert(); });

    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, [this]() {
        if (--m_countdown <= 0)
            revert();
        else
            syncFooter();
    });
}

void DisplaysWindow::openOn(bool reload)
{
    if (reload) {
        m_error.clear();
        m_selected = -1;
        load();
    }
    present();
}

void DisplaysWindow::dismiss()
{
    if (m_pending)
        revert();
    CenteredOverlay::dismiss();
}

void DisplaysWindow::outputsSettled()
{
    if (!isVisible() || busy())
        return;
    /* A monitor plugged in or out, or changed by something else: show what
     * is live now, unless there are unapplied edits for the same outputs. */
    const QVector<Output> live = Displays::query();
    if (Displays::key(live) != Displays::key(m_outputs) || !dirty())
        load();
}

void DisplaysWindow::load()
{
    /* Stay on the output being looked at; else start on the primary. */
    const QString was = selected() ? selected()->name : QString();
    m_outputs = Displays::query();
    m_before = m_outputs;
    m_selected = 0;
    for (int i = 0; i < m_outputs.size(); i++)
        if (m_outputs[i].primary)
            m_selected = i;
    for (int i = 0; i < m_outputs.size(); i++)
        if (!was.isEmpty() && m_outputs[i].name == was)
            m_selected = i;
    rebuildChips();
    syncAll();
}

void DisplaysWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (m_pending)
            keep();
        else if (dirty() && !m_running)
            apply();
        return;
    }
    CenteredOverlay::keyPressEvent(e);
}

Output *DisplaysWindow::selected()
{
    if (m_selected < 0 || m_selected >= m_outputs.size())
        return nullptr;
    return &m_outputs[m_selected];
}

bool DisplaysWindow::dirty() const
{
    return Displays::profile(m_outputs) != Displays::profile(m_before);
}

void DisplaysWindow::rebuildChips()
{
    while (m_chipRow->count() > 1) {
        QLayoutItem *it = m_chipRow->takeAt(0);
        if (it->widget()) {
            it->widget()->hide();
            it->widget()->deleteLater();
        }
        delete it;
    }
    m_chips.clear();
    for (int i = 0; i < m_outputs.size(); i++) {
        auto *chip = new ShellButton(m_outputs[i].name, m_chipRow->parentWidget());
        chip->setActive(i == m_selected);
        connect(chip, &ShellButton::activated, this, [this, i]() {
            m_selected = i;
            rebuildChips();
            syncDetails();
            m_view->update();
        });
        m_chipRow->insertWidget(i, chip);
        m_chips << chip;
    }
}

void DisplaysWindow::syncAll()
{
    m_view->update();
    for (int i = 0; i < m_chips.size(); i++)
        m_chips[i]->setActive(i == m_selected);
    syncDetails();
    syncFooter();
}

void DisplaysWindow::syncDetails()
{
    const Output *o = selected();
    m_heading->setText(o ? o->name : QStringLiteral("No outputs"));
    m_heading->setColor(o && o->enabled ? Theme::textStrong : Theme::textMuted);

    const bool on = o && o->enabled;
    m_enabled->setLabel(on ? QStringLiteral("On") : QStringLiteral("Off"));
    m_enabled->setActive(on);
    m_enabled->setEnabled(o && !m_pending && !m_running);
    m_primary->setActive(on && o->primary);
    m_primary->setEnabled(on && !m_pending && !m_running);

    QStringList sizes, rates;
    int sizeIndex = -1, rateIndex = -1;
    if (o) {
        for (const Mode &m : o->modes) {
            const QString label =
                QStringLiteral("%1 × %2").arg(m.w).arg(m.h)
                + (m.w == o->preferred.w && m.h == o->preferred.h
                       ? QStringLiteral("  (native)")
                       : QString());
            if (!sizes.contains(label))
                sizes << label;
            if (m.w == o->w && m.h == o->h) {
                sizeIndex = sizes.size() - 1;
                if (sameRate(m.rate, o->rate))
                    rateIndex = rates.size();
                rates << QStringLiteral("%1 Hz").arg(m.rate, 0, 'f', 2);
            }
        }
    }
    m_resolution->setChoices(sizes, sizeIndex);
    m_rate->setChoices(rates, rateIndex);
    m_rotation->setChoices(rotationLabels,
                           o ? int(rotations.indexOf(o->rotate)) : -1);
    const bool tearFree = on && !o->tearFree.isEmpty();
    m_tearFreeLabel->setVisible(tearFree);
    m_tearFree->setVisible(tearFree);
    m_tearFree->setChoices(tearFreeLabels,
                           tearFree ? int(tearFreeModes.indexOf(o->tearFree)) : -1);
    /* Scale and adaptive sync: Hyprland's, so only there. */
    const bool hypr = on && Surface::wayland();
    QVector<double> offered = scales;
    if (o && !offered.contains(o->scale))
        offered << o->scale;
    std::sort(offered.begin(), offered.end());
    QStringList scaleLabels;
    for (double v : offered)
        scaleLabels << scaleLabel(v);
    m_scaleLabel->setVisible(hypr);
    m_scale->setVisible(hypr);
    m_scale->setChoices(scaleLabels, o ? int(offered.indexOf(o->scale)) : -1);
    const bool vrr = hypr && !o->vrr.isEmpty();
    m_vrrLabel->setVisible(vrr);
    m_vrr->setVisible(vrr);
    m_vrr->setChoices(vrrLabels, vrr ? int(vrrModes.indexOf(o->vrr)) : -1);
    for (QWidget *w : {static_cast<QWidget *>(m_resolution),
                       static_cast<QWidget *>(m_rate),
                       static_cast<QWidget *>(m_rotation),
                       static_cast<QWidget *>(m_tearFree),
                       static_cast<QWidget *>(m_scale),
                       static_cast<QWidget *>(m_vrr)})
        w->setEnabled(on && !m_pending && !m_running);

    syncVibrance();
}

void DisplaysWindow::syncVibrance()
{
    const Output *o = selected();
    const double s =
        o && o->enabled ? Vibrance::instance()->saturation(o->name) : -1;
    m_vibranceLabel->setVisible(s >= 0);
    m_vibranceRow->setVisible(s >= 0);
    if (s >= 0) {
        m_vibrance->setValue(s / Vibrance::maximum);
        m_vibrance->setFillColor(std::fabs(s - Vibrance::normal) < 0.005
                                     ? Theme::textMuted
                                     : Theme::accent);
        m_vibrancePct->setText(QStringLiteral("%1%").arg(qRound(s * 100)));
    }
    if (isVisible())
        layoutBox();
}

void DisplaysWindow::syncFooter()
{
    m_resetButton->setVisible(!m_pending);
    m_applyButton->setVisible(!m_pending);
    m_revertButton->setVisible(m_pending);
    m_keepButton->setVisible(m_pending);

    const bool canApply = dirty() && !m_running;
    m_resetButton->setEnabled(canApply);
    m_applyButton->setEnabled(canApply);
    m_applyButton->setActive(canApply);
    m_keepButton->setActive(true);
    m_revertButton->setEnabled(!m_running);
    m_keepButton->setEnabled(!m_running);

    if (m_pending) {
        m_status->setColor(Theme::text);
        m_status->setText(QStringLiteral("Keep this layout? Reverting in %1 s")
                              .arg(m_countdown));
    } else if (!m_error.isEmpty()) {
        m_status->setColor(Theme::danger);
        m_status->setText(m_error);
    } else if (m_running) {
        m_status->setColor(Theme::textMuted);
        m_status->setText(QStringLiteral("Applying…"));
    } else {
        m_status->setColor(Theme::textMuted);
        m_status->setText(dirty() ? QStringLiteral("Unapplied changes")
                                  : QString());
    }
}

/* Change the selected output's size (mode or rotation) in place, sliding
 * everything to its right or below by the difference so neighbours stay
 * flush instead of overlapping or leaving a gap. */
void DisplaysWindow::resizeSelected(int w, int h, const QString &rotate,
                                    double scale)
{
    Output *o = selected();
    if (!o)
        return;
    const QRect before = o->rect();
    o->w = w;
    o->h = h;
    o->rotate = rotate;
    if (scale > 0)
        o->scale = scale;
    const QRect after = o->rect();
    const int dw = after.width() - before.width();
    const int dh = after.height() - before.height();
    for (int i = 0; i < m_outputs.size(); i++) {
        Output &q = m_outputs[i];
        if (i == m_selected || !q.enabled)
            continue;
        if (q.x >= before.x() + before.width())
            q.x += dw;
        if (q.y >= before.y() + before.height())
            q.y += dh;
    }
    normalize(m_outputs);
    syncAll();
}

void DisplaysWindow::setOutputOn(bool on)
{
    Output *o = selected();
    if (!o || o->enabled == on)
        return;
    m_error.clear();
    if (!on) {
        if (enabledCount(m_outputs) <= 1) {
            m_error = QStringLiteral("At least one display has to stay on");
            syncFooter();
            return;
        }
        o->enabled = false;
        if (o->primary) {
            o->primary = false;
            for (Output &q : m_outputs)
                if (q.enabled) {
                    q.primary = true;
                    break;
                }
        }
    } else {
        /* Back at its native mode, to the right of everything. */
        int right = 0;
        for (const Output &q : m_outputs)
            if (q.enabled)
                right = qMax(right, q.x + q.width());
        o->enabled = true;
        o->w = o->preferred.w;
        o->h = o->preferred.h;
        o->rate = o->preferred.rate;
        o->rotate = QStringLiteral("normal");
        o->x = right;
        o->y = 0;
    }
    normalize(m_outputs);
    syncAll();
}

void DisplaysWindow::apply()
{
    m_error.clear();
    m_running = true;
    syncAll();
    DisplaySettings::instance()->applyLayout(
        m_outputs, [this](int code, const QString &err) {
            m_running = false;
            if (code != 0) {
                const QString why = err.trimmed().section(QLatin1Char('\n'), 0, 0);
                const QString who = Surface::wayland() ? QStringLiteral("Hyprland")
                                                       : QStringLiteral("xrandr");
                m_error = why.isEmpty()
                              ? who + QStringLiteral(" refused this layout")
                              : who + QStringLiteral(": ") + why;
                revert();
                return;
            }
            m_pending = true;
            m_countdown = confirmSeconds;
            m_tick.start();
            syncAll();
            QTimer::singleShot(300, this, [this]() { reanchor(); });
        });
}

void DisplaysWindow::keep()
{
    m_tick.stop();
    m_pending = false;
    DisplaySettings::instance()->saveLayout(m_outputs);
    m_before = m_outputs;
    syncAll();
}

void DisplaysWindow::revert()
{
    m_tick.stop();
    m_pending = false;
    m_outputs = m_before;
    m_running = true;
    syncAll();
    DisplaySettings::instance()->applyLayout(
        m_before, [this](int, const QString &) {
            m_running = false;
            syncAll();
            QTimer::singleShot(300, this, [this]() { reanchor(); });
        });
}

/* -------------------------------------------------------- DisplaySettings */

namespace {

class RandrFilter : public QAbstractNativeEventFilter {
public:
    explicit RandrFilter(uint8_t firstEvent) : m_first(firstEvent) {}

    bool nativeEventFilter(const QByteArray &type, void *message,
                           qintptr *) override
    {
        if (type != "xcb_generic_event_t")
            return false;
        const auto *ev = static_cast<const xcb_generic_event_t *>(message);
        const uint8_t t = ev->response_type & 0x7f;
        if (t == m_first + XCB_RANDR_NOTIFY) {
            /* Property changes are vibrance's CTM writes, not layout. */
            const auto *n = static_cast<const xcb_randr_notify_event_t *>(message);
            if (n->subCode == XCB_RANDR_NOTIFY_OUTPUT_PROPERTY)
                return false;
            DisplaySettings::instance()->randrEvent();
        } else if (t == m_first + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            DisplaySettings::instance()->randrEvent();
        }
        return false;
    }

private:
    uint8_t m_first;
};

} /* namespace */

DisplaySettings *DisplaySettings::instance()
{
    static DisplaySettings *s = new DisplaySettings();
    return s;
}

DisplaySettings::DisplaySettings(QObject *parent) : QObject(parent)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(settleMs);
    connect(&m_settle, &QTimer::timeout, this, &DisplaySettings::settle);

    m_lastKey = Displays::key(Displays::query());

    if (Surface::wayland()) {
        /* Hyprland reports monitors coming and going, and a reload that
         * put its own monitor rules back. */
        WmState *wm = WmState::instance();
        connect(wm, &WmState::outputsChanged, this, &DisplaySettings::randrEvent);
        connect(wm, &WmState::configReloaded, this, &DisplaySettings::outputsReset);
        /* There is no .xinitrc to have applied it: the saved layout for
         * these outputs, now. */
        outputsReset();
        return;
    }

    xcb_connection_t *c = conn();
    if (!c)
        return;
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_randr_id);
    if (!ext || !ext->present)
        return;
    /* Qt selects these on the root itself; selecting the same set again is
     * harmless and does not depend on that. */
    xcb_randr_select_input(c, rootWindow(c),
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
                               | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
                               | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
                               | XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);
    xcb_flush(c);
    qApp->installNativeEventFilter(new RandrFilter(ext->first_event));
}

void DisplaySettings::randrEvent()
{
    m_settle.start();
}

void DisplaySettings::outputsReset()
{
    m_forceApply = true;
    m_settle.start();
}

/* The connected set changed: apply what was saved for the new one. The
 * layout's own changes also land here, but leave the key as it was. */
void DisplaySettings::settle()
{
    /* A modeset can reset the gamma ramps. */
    NightLight::instance()->refresh();
    if (m_win)
        m_win->outputsSettled();
    const QString key = Displays::key(Displays::query());
    const bool force = m_forceApply;
    m_forceApply = false;
    if (key == m_lastKey && !force)
        return;
    m_lastKey = key;
    if (m_win && m_win->busy())
        return;
    runScript({QStringLiteral("apply")}, [](int, const QString &) {
        Vibrance::instance()->refresh();
    });
}

void DisplaySettings::openWindow()
{
    if (!m_win)
        m_win = new DisplaysWindow();
    m_win->openOn(true);
}

void DisplaySettings::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void DisplaySettings::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}

QString DisplaySettings::writeTemp(const QString &name, const QString &text)
{
    QString dir =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    const QString path = dir + QLatin1Char('/') + name;
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(text.toUtf8());
        file.commit();
    }
    return path;
}

void DisplaySettings::applyLayout(
    const QVector<Output> &outputs,
    std::function<void(int, const QString &)> done)
{
    const QString path = writeTemp(QStringLiteral("fanshell-qs-outputs-trial.conf"),
                                   Displays::profile(outputs));
    runScript({QStringLiteral("apply"), path},
              [done](int code, const QString &err) {
                  Vibrance::instance()->refresh();
                  if (done)
                      done(code, err);
              });
}

void DisplaySettings::saveLayout(const QVector<Output> &outputs)
{
    const QString path = writeTemp(QStringLiteral("fanshell-qs-outputs-keep.conf"),
                                   Displays::profile(outputs));
    runScript({QStringLiteral("save"), path}, [](int code, const QString &err) {
        if (code != 0)
            qWarning("fanshell-qs: saving the output layout failed: %s",
                     qPrintable(err.trimmed()));
    });
}

void DisplaySettings::runScript(const QStringList &args,
                                std::function<void(int, const QString &)> done)
{
    auto *proc = new QProcess(this);
    proc->setProgram(QStringLiteral("fanshell-qs-outputs"));
    proc->setArguments(args);
    proc->setChildProcessModifier([]() {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
    });
    connect(proc, &QProcess::finished, this, [proc, done]() {
        const int code = proc->exitStatus() == QProcess::NormalExit
                             ? proc->exitCode()
                             : -1;
        if (done)
            done(code, QString::fromUtf8(proc->readAllStandardError()));
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this,
            [proc, done](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart)
                    return;
                if (done)
                    done(-1, QStringLiteral("fanshell-qs-outputs is not installed"));
                proc->deleteLater();
            });
    proc->start();
}

/* ------------------------------------------------------------ ScreenPower */

namespace {

constexpr auto dpmsConf = "dpms.conf";

using Settings = ScreenPower::Settings;

/* X refuses DPMS timeouts that decrease from one enabled stage to the next. */
Settings ordered(Settings s)
{
    int floor = 0;
    for (int *t : {&s.standby, &s.suspend, &s.off}) {
        if (*t > 0 && *t < floor)
            *t = floor;
        if (*t > 0)
            floor = *t;
    }
    return s;
}

QString durationLabel(int seconds)
{
    if (seconds <= 0)
        return QStringLiteral("Never");
    if (seconds % 3600 == 0)
        return QStringLiteral("%1 h").arg(seconds / 3600);
    if (seconds % 60 == 0)
        return QStringLiteral("%1 min").arg(seconds / 60);
    return QStringLiteral("%1 s").arg(seconds);
}

/* The preset durations, plus `current` if it is something else (set by
 * xset, say), so the selector can always show what is live. */
QVector<int> durationChoices(int current)
{
    QVector<int> v{0, 60, 120, 180, 300, 600, 900, 1200, 1800, 2700, 3600, 7200};
    if (!v.contains(current)) {
        v << current;
        std::sort(v.begin(), v.end());
    }
    return v;
}

} /* namespace */

class ScreenPowerWindow : public CenteredOverlay {
public:
    ScreenPowerWindow()
        : CenteredOverlay(QStringLiteral("fanshell-screen-power"),
                          QStringLiteral("fanshell-qs-screen-power"), 440)
    {
        auto *lay = new QVBoxLayout(box());
        lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                Theme::popupMargin, Theme::popupMargin);
        lay->setSpacing(Theme::popupSpacing);

        auto *title = new TextItem(box());
        title->setPixelSize(Theme::bodyFontSize);
        title->setBold(true);
        title->setColor(Theme::textStrong);
        title->setText(QStringLiteral("Screen power"));
        lay->addWidget(title);

        auto *grid = new QGridLayout();
        grid->setHorizontalSpacing(Theme::sectionSpacing);
        grid->setVerticalSpacing(Theme::rowSpacing);
        grid->setColumnStretch(1, 1);

        m_blank = new Selector(choiceList(), box());
        m_dpms = new ShellButton(QStringLiteral("Off"), box());
        auto *dpmsRow = new QWidget(box());
        auto *dr = new QHBoxLayout(dpmsRow);
        dr->setContentsMargins(0, 0, 0, 0);
        dr->addWidget(m_dpms);
        dr->addStretch(1);
        m_standby = new Selector(choiceList(), box());
        m_suspend = new Selector(choiceList(), box());
        m_off = new Selector(choiceList(), box());

        grid->addWidget(fieldLabel(QStringLiteral("Blank after"), box()), 0, 0);
        grid->addWidget(m_blank, 0, 1);
        grid->addWidget(new HLine(box()), 1, 0, 1, 2);
        grid->addWidget(fieldLabel(QStringLiteral("Power saving"), box()), 2, 0);
        grid->addWidget(dpmsRow, 2, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Standby after"), box()), 3, 0);
        grid->addWidget(m_standby, 3, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Suspend after"), box()), 4, 0);
        grid->addWidget(m_suspend, 4, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Off after"), box()), 5, 0);
        grid->addWidget(m_off, 5, 1);
        lay->addLayout(grid);
        /* Wayland outputs are on or off: one timeout, no stages. */
        const bool wayland = Surface::wayland();
        if (wayland) {
            if (auto *label = qobject_cast<TextItem *>(
                    grid->itemAtPosition(0, 0)->widget()))
                label->setText(QStringLiteral("Turn off after"));
            for (int row = 1; row <= 5; ++row)
                for (int col = 0; col < 2; ++col)
                    if (QLayoutItem *it = grid->itemAtPosition(row, col))
                        if (it->widget())
                            it->widget()->hide();
        }

        auto *note = new QWidget(box());
        auto *nl = new QVBoxLayout(note);
        nl->setContentsMargins(0, 0, 0, 0);
        nl->setSpacing(2);
        const QList<const char *> lines =
            wayland ? QList<const char *>{"The displays power down after this long "
                                          "without input,",
                                          "unless a program asks the screen to stay on."}
                    : QList<const char *>{"Blanking paints the screen black.",
                                          "Power saving (DPMS) powers the monitor down in stages;",
                                          "most monitors treat standby, suspend and off alike."};
        for (const char *line : lines) {
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
        fl->setSpacing(Theme::rowSpacing);
        auto *offNow = new ShellButton(QStringLiteral("Turn off now"), footer);
        auto *close = new ShellButton(QStringLiteral("Close"), footer);
        fl->addWidget(offNow);
        fl->addStretch(1);
        fl->addWidget(close);
        lay->addWidget(footer);

        auto bind = [this](Selector *sel, int Settings::*field) {
            sel->chosen = [this, sel, field](int i) {
                const QVector<int> v = durationChoices(m_settings.*field);
                if (i < 0 || i >= v.size())
                    return;
                /* set() raises any later stage that would now come first. */
                Settings s = m_settings;
                s.*field = v[i];
                change(s);
            };
        };
        bind(m_blank, &Settings::blank);
        bind(m_standby, &Settings::standby);
        bind(m_suspend, &Settings::suspend);
        bind(m_off, &Settings::off);
        connect(m_dpms, &ShellButton::activated, this, [this]() {
            Settings s = m_settings;
            s.dpms = !s.dpms;
            change(s);
        });
        connect(offNow, &ShellButton::activated, this, [this]() {
            dismiss();
            ScreenPower::instance()->turnOffNow();
        });
        connect(close, &ShellButton::activated, this, [this]() { dismiss(); });
    }

    void open()
    {
        m_settings = ScreenPower::instance()->live();
        sync();
        present();
    }

private:
    void change(const Settings &s)
    {
        ScreenPower::instance()->set(s);
        m_settings = ScreenPower::instance()->live();
        sync();
    }

    void sync()
    {
        auto fill = [](Selector *sel, int value) {
            QStringList labels;
            const QVector<int> v = durationChoices(value);
            for (int d : v)
                labels << durationLabel(d);
            sel->setChoices(labels, int(v.indexOf(value)));
        };
        fill(m_blank, m_settings.blank);
        fill(m_standby, m_settings.standby);
        fill(m_suspend, m_settings.suspend);
        fill(m_off, m_settings.off);
        m_dpms->setLabel(m_settings.dpms ? QStringLiteral("On")
                                         : QStringLiteral("Off"));
        m_dpms->setActive(m_settings.dpms);
        for (Selector *sel : {m_standby, m_suspend, m_off})
            sel->setEnabled(m_settings.dpms);
    }

    Settings m_settings;
    Selector *m_blank;
    ShellButton *m_dpms;
    Selector *m_standby;
    Selector *m_suspend;
    Selector *m_off;
};

ScreenPower *ScreenPower::instance()
{
    static ScreenPower *s = new ScreenPower();
    return s;
}

ScreenPower::ScreenPower(QObject *parent) : QObject(parent)
{
    m_wake.setInterval(1000);
    connect(&m_wake, &QTimer::timeout, this, [this]() {
        xcb_connection_t *c = conn();
        if (!c)
            return;
        xcb_dpms_info_reply_t *info =
            xcb_dpms_info_reply(c, xcb_dpms_info(c), nullptr);
        const bool awake =
            !info || info->power_level == XCB_DPMS_DPMS_MODE_ON;
        free(info);
        if (!awake)
            return;
        m_wake.stop();
        if (m_enabledForOff) {
            m_enabledForOff = false;
            xcb_dpms_disable(c);
            xcb_flush(c);
        }
    });

    /* Settle the session on the saved settings, or on the defaults
     * .xinitrc used to set with xset. */
    const QHash<QString, QString> conf =
        Paths::readConf(QLatin1String(dpmsConf));
    Settings s;
    s.blank = conf.value(QStringLiteral("blank"), QStringLiteral("0")).toInt();
    s.dpms = conf.value(QStringLiteral("dpms")) == QLatin1String("on");
    s.standby = conf.value(QStringLiteral("standby"), QStringLiteral("0")).toInt();
    s.suspend = conf.value(QStringLiteral("suspend"), QStringLiteral("0")).toInt();
    s.off = conf.value(QStringLiteral("off"), QStringLiteral("0")).toInt();
    apply(ordered(s));
}

Settings ScreenPower::live() const
{
    if (Surface::wayland())
        return m_current;
    Settings s;
    xcb_connection_t *c = conn();
    if (!c)
        return s;
    if (auto *ss = xcb_get_screen_saver_reply(c, xcb_get_screen_saver(c),
                                              nullptr)) {
        s.blank = ss->timeout;
        free(ss);
    }
    /* A request to an extension the server lacks (Xvfb, Xephyr, some VMs
     * have no DPMS) makes it drop the whole connection. */
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_dpms_id);
    if (!ext || !ext->present)
        return s;
    if (auto *info = xcb_dpms_info_reply(c, xcb_dpms_info(c), nullptr)) {
        s.dpms = info->state;
        free(info);
    }
    if (auto *t = xcb_dpms_get_timeouts_reply(c, xcb_dpms_get_timeouts(c),
                                              nullptr)) {
        s.standby = t->standby_timeout;
        s.suspend = t->suspend_timeout;
        s.off = t->off_timeout;
        free(t);
    }
    return s;
}

void ScreenPower::apply(const Settings &s)
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* Wayland: an idle notification, after which the outputs power off
     * until the next input. Programs that inhibit idling hold it off. */
    if (Surface::wayland()) {
        m_current = s;
        delete m_idleOff;
        m_idleOff = nullptr;
        if (s.blank > 0 && Wl::IdleWatch::supported()) {
            m_idleOff = new Wl::IdleWatch(s.blank * 1000, false, this);
            connect(m_idleOff, &Wl::IdleWatch::idled, this, &ScreenPower::idleOff);
            connect(m_idleOff, &Wl::IdleWatch::resumed, this, [this]() {
                if (m_poweredOff)
                    Wl::Client::instance()->setPower(true);
                m_poweredOff = false;
            });
        }
        return;
    }
#endif
    xcb_connection_t *c = conn();
    if (!c)
        return;
    /* Blanking, not a screensaver picture; no cycling. */
    xcb_set_screen_saver(c, int16_t(s.blank), 0, XCB_BLANKING_PREFERRED,
                         XCB_EXPOSURES_DEFAULT);
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_dpms_id);
    if (ext && ext->present) {
        xcb_dpms_set_timeouts(c, uint16_t(s.standby), uint16_t(s.suspend),
                              uint16_t(s.off));
        if (s.dpms)
            xcb_dpms_enable(c);
        else
            xcb_dpms_disable(c);
    }
    xcb_flush(c);
}

#ifdef FANSHELL_QS_HAVE_WAYLAND
/* Idle long enough: off, unless a program asked over D-Bus for the screen
 * to stay on (which the compositor knows nothing of); then look again in a
 * while, for as long as the idle lasts. */
void ScreenPower::idleOff()
{
    if (!m_idleOff || !m_idleOff->idle() || m_poweredOff)
        return;
    if (!IdleInhibitor::instance()->reasons().isEmpty()) {
        QTimer::singleShot(5000, this, &ScreenPower::idleOff);
        return;
    }
    m_poweredOff = Wl::Client::instance()->setPower(false);
}
#endif

void ScreenPower::set(Settings s)
{
    s = ordered(s);
    m_enabledForOff = false;
    apply(s);
    save(s);
}

void ScreenPower::save(const Settings &s)
{
    if (!Paths::writeConf(
            QLatin1String(dpmsConf),
            QStringLiteral("Screen blanking and DPMS, set from the Screen power "
                           "popup.\nSeconds of idle; 0 = never."),
            {QStringLiteral("blank=%1").arg(s.blank),
             QStringLiteral("dpms=%1").arg(s.dpms ? "on" : "off"),
             QStringLiteral("standby=%1").arg(s.standby),
             QStringLiteral("suspend=%1").arg(s.suspend),
             QStringLiteral("off=%1").arg(s.off)}))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(dpmsConf))));
}

void ScreenPower::turnOffNow()
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* Off once input has been still for a moment -- or the click that asked
     * would wake them at once -- and on at the next input. */
    if (Surface::wayland()) {
        if (!Wl::IdleWatch::supported())
            return;
        delete m_offNow;
        m_offNow = new Wl::IdleWatch(700, true, this);
        connect(m_offNow, &Wl::IdleWatch::idled, this,
                []() { Wl::Client::instance()->setPower(false); });
        connect(m_offNow, &Wl::IdleWatch::resumed, this, [this]() {
            Wl::Client::instance()->setPower(true);
            m_offNow->deleteLater();
            m_offNow = nullptr;
        });
        return;
    }
#endif
    xcb_connection_t *c = conn();
    if (!c)
        return;
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_dpms_id);
    if (!ext || !ext->present)
        return;
    /* Forcing a level needs DPMS on; switch it on for now if it is off, and
     * back off once the displays wake. */
    if (!live().dpms) {
        xcb_dpms_enable(c);
        m_enabledForOff = true;
    }
    xcb_flush(c);
    /* After the click or key that asked for it has been released, or that
     * very event would wake the displays again. */
    QTimer::singleShot(700, this, [this]() {
        xcb_connection_t *c = conn();
        if (!c)
            return;
        xcb_dpms_force_level(c, XCB_DPMS_DPMS_MODE_OFF);
        xcb_flush(c);
        m_wake.start();
    });
}

void ScreenPower::openWindow()
{
    if (!m_win)
        m_win = new ScreenPowerWindow();
    m_win->open();
}

void ScreenPower::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void ScreenPower::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}

/* ------------------------------------------------------------- ScreenLock */

namespace {

constexpr auto lockConf = "lock.conf";

using LockSettings = ScreenLock::Settings;

/* Whether the focused window (_NET_ACTIVE_WINDOW) is fullscreen. */
bool fullscreenFocused(xcb_connection_t *c)
{
    const xcb_atom_t active = atom(c, "_NET_ACTIVE_WINDOW");
    const xcb_atom_t state = atom(c, "_NET_WM_STATE");
    const xcb_atom_t full = atom(c, "_NET_WM_STATE_FULLSCREEN");
    if (!active || !state || !full)
        return false;
    xcb_window_t window = XCB_WINDOW_NONE;
    if (auto *r = xcb_get_property_reply(
            c, xcb_get_property(c, 0, rootWindow(c), active, XCB_ATOM_WINDOW, 0, 1),
            nullptr)) {
        if (xcb_get_property_value_length(r) >= int(sizeof(xcb_window_t)))
            window = *static_cast<xcb_window_t *>(xcb_get_property_value(r));
        free(r);
    }
    if (window == XCB_WINDOW_NONE)
        return false;
    bool fullscreen = false;
    if (auto *r = xcb_get_property_reply(
            c, xcb_get_property(c, 0, window, state, XCB_ATOM_ATOM, 0, 32),
            nullptr)) {
        const auto *atoms = static_cast<xcb_atom_t *>(xcb_get_property_value(r));
        const int n = xcb_get_property_value_length(r) / int(sizeof(xcb_atom_t));
        for (int i = 0; i < n; i++)
            fullscreen = fullscreen || atoms[i] == full;
        free(r);
    }
    return fullscreen;
}

} /* namespace */

class ScreenLockWindow : public CenteredOverlay {
public:
    ScreenLockWindow()
        : CenteredOverlay(QStringLiteral("fanshell-screen-lock"),
                          QStringLiteral("fanshell-qs-screen-lock"), 440)
    {
        auto *lay = new QVBoxLayout(box());
        lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                Theme::popupMargin, Theme::popupMargin);
        lay->setSpacing(Theme::popupSpacing);

        auto *title = new TextItem(box());
        title->setPixelSize(Theme::bodyFontSize);
        title->setBold(true);
        title->setColor(Theme::textStrong);
        title->setText(QStringLiteral("Screen lock"));
        lay->addWidget(title);

        auto *grid = new QGridLayout();
        grid->setHorizontalSpacing(Theme::sectionSpacing);
        grid->setVerticalSpacing(Theme::rowSpacing);
        grid->setColumnStretch(1, 1);

        auto toggleRow = [this](ShellButton **button) {
            *button = new ShellButton(QStringLiteral("Off"), box());
            auto *row = new QWidget(box());
            auto *rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->addWidget(*button);
            rl->addStretch(1);
            return row;
        };
        m_after = new Selector(choiceList(), box());
        grid->addWidget(fieldLabel(QStringLiteral("Lock after"), box()), 0, 0);
        grid->addWidget(m_after, 0, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Before sleep"), box()), 1, 0);
        grid->addWidget(toggleRow(&m_suspend), 1, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Pause for fullscreen"), box()),
                        2, 0);
        grid->addWidget(toggleRow(&m_fullscreen), 2, 1);
        grid->addWidget(fieldLabel(QStringLiteral("Pause while audio plays"), box()),
                        3, 0);
        grid->addWidget(toggleRow(&m_audio), 3, 1);
        grid->addWidget(new HLine(box()), 4, 0, 1, 2);
        m_displayOff = new Selector(choiceList(), box());
        grid->addWidget(fieldLabel(QStringLiteral("Display off when locked"), box()),
                        5, 0);
        grid->addWidget(m_displayOff, 5, 1);
        lay->addLayout(grid);

        m_pausedNow = new TextItem(box());
        m_pausedNow->setPixelSize(Theme::tinyFontSize);
        m_pausedNow->setColor(Theme::accent);
        m_pausedNow->setElide(true);
        lay->addWidget(m_pausedNow);

        auto *note = new QWidget(box());
        auto *nl = new QVBoxLayout(note);
        nl->setContentsMargins(0, 0, 0, 0);
        nl->setSpacing(2);
        for (const char *line : {"Idle is time without keyboard or mouse input.",
                                 "Pause for fullscreen waits while a video or "
                                 "game fills the screen.",
                                 "Programs that ask -- a browser playing video, a "
                                 "call -- pause it too.",
                                 "Display off counts from the last key or move on "
                                 "the lock screen."}) {
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
        fl->setSpacing(Theme::rowSpacing);
        auto *lockNow = new ShellButton(QStringLiteral("Lock now"), footer);
        auto *close = new ShellButton(QStringLiteral("Close"), footer);
        fl->addWidget(lockNow);
        fl->addStretch(1);
        fl->addWidget(close);
        lay->addWidget(footer);

        m_after->chosen = [this](int i) {
            const QVector<int> v = durationChoices(m_settings.after);
            if (i < 0 || i >= v.size())
                return;
            LockSettings s = m_settings;
            s.after = v[i];
            change(s);
        };
        m_displayOff->chosen = [this](int i) {
            const QVector<int> v = durationChoices(m_settings.displayOff);
            if (i < 0 || i >= v.size())
                return;
            LockSettings s = m_settings;
            s.displayOff = v[i];
            change(s);
        };
        connect(m_suspend, &ShellButton::activated, this, [this]() {
            LockSettings s = m_settings;
            s.beforeSuspend = !s.beforeSuspend;
            change(s);
        });
        connect(m_fullscreen, &ShellButton::activated, this, [this]() {
            LockSettings s = m_settings;
            s.skipFullscreen = !s.skipFullscreen;
            change(s);
        });
        connect(m_audio, &ShellButton::activated, this, [this]() {
            LockSettings s = m_settings;
            s.skipAudio = !s.skipAudio;
            change(s);
        });
        /* The "Paused now" line follows along while the window is open. */
        connect(IdleInhibitor::instance(), &IdleInhibitor::changed, this,
                [this]() { if (isVisible()) sync(); });
        connect(PulseBackend::instance(), &PulseBackend::playingChanged, this,
                [this]() { if (isVisible()) sync(); });
        connect(lockNow, &ShellButton::activated, this, [this]() {
            dismiss();
            /* After this popup has let go of the keyboard, which the lock
             * screen needs to grab. */
            QTimer::singleShot(300, ScreenLock::instance(),
                               []() { ScreenLock::instance()->lockNow(); });
        });
        connect(close, &ShellButton::activated, this, [this]() { dismiss(); });
    }

    void open()
    {
        m_settings = ScreenLock::instance()->settings();
        sync();
        present();
    }

private:
    void change(const LockSettings &s)
    {
        ScreenLock::instance()->set(s);
        m_settings = s;
        sync();
    }

    void sync()
    {
        auto fill = [](Selector *sel, int value) {
            QStringList labels;
            const QVector<int> v = durationChoices(value);
            for (int d : v)
                labels << durationLabel(d);
            sel->setChoices(labels, int(v.indexOf(value)));
        };
        fill(m_after, m_settings.after);
        fill(m_displayOff, m_settings.displayOff);
        auto onOff = [](ShellButton *b, bool on) {
            b->setLabel(on ? QStringLiteral("On") : QStringLiteral("Off"));
            b->setActive(on);
        };
        onOff(m_suspend, m_settings.beforeSuspend);
        onOff(m_fullscreen, m_settings.skipFullscreen);
        m_fullscreen->setEnabled(m_settings.after > 0);
        onOff(m_audio, m_settings.skipAudio);
        m_audio->setEnabled(m_settings.after > 0);
        const QStringList paused = ScreenLock::instance()->pausedBy();
        m_pausedNow->setText(paused.isEmpty() || m_settings.after <= 0
                                 ? QString()
                                 : QStringLiteral("Paused now: ") + paused.join(QStringLiteral("; ")));
        m_pausedNow->setToolTip(m_pausedNow->text());
        m_pausedNow->setVisible(!m_pausedNow->text().isEmpty());
        if (isVisible())
            layoutBox();
    }

    LockSettings m_settings;
    Selector *m_after;
    Selector *m_displayOff;
    ShellButton *m_suspend;
    ShellButton *m_fullscreen;
    ShellButton *m_audio;
    TextItem *m_pausedNow;
};

ScreenLock *ScreenLock::instance()
{
    static ScreenLock *s = new ScreenLock();
    return s;
}

ScreenLock::ScreenLock(QObject *parent) : QObject(parent)
{
    const QHash<QString, QString> conf =
        Paths::readConf(QLatin1String(lockConf));
    m_settings.after =
        conf.value(QStringLiteral("after"), QString::number(m_settings.after))
            .toInt();
    if (conf.contains(QStringLiteral("before_suspend")))
        m_settings.beforeSuspend =
            conf.value(QStringLiteral("before_suspend")) == QLatin1String("on");
    if (conf.contains(QStringLiteral("skip_fullscreen")))
        m_settings.skipFullscreen =
            conf.value(QStringLiteral("skip_fullscreen")) == QLatin1String("on");
    if (conf.contains(QStringLiteral("skip_audio")))
        m_settings.skipAudio =
            conf.value(QStringLiteral("skip_audio")) == QLatin1String("on");
    m_settings.displayOff =
        conf.value(QStringLiteral("display_off"), QString::number(m_settings.displayOff))
            .toInt();

    m_age.start();
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, &ScreenLock::tick);

    QDBusConnection::systemBus().connect(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("PrepareForSleep"), this, SLOT(prepareForSleep(bool)));
    if (m_settings.beforeSuspend)
        takeSleepInhibitor();

    m_lockWatch.setInterval(1000);
    connect(&m_lockWatch, &QTimer::timeout, this, [this]() {
        if (!LockScreen::isLocked())
            setLocked(false);
    });
    /* Locked before this shell started (it was restarted, say). */
    if (LockScreen::isLocked())
        setLocked(true);
    tick();
}

void ScreenLock::setLocked(bool locked)
{
    if (locked == m_locked)
        return;
    m_locked = locked;
    if (locked)
        m_lockWatch.start();
    else
        m_lockWatch.stop();
    emit lockedChanged(locked);
}

void ScreenLock::set(const Settings &s)
{
    m_settings = s;
    if (!Paths::writeConf(
            QLatin1String(lockConf),
            QStringLiteral("Automatic screen lock, set from the Screen lock "
                           "popup.\nafter: seconds idle; 0 = never.\n"
                           "display_off: seconds on the lock screen before the "
                           "displays power down; 0 = never."),
            {QStringLiteral("after=%1").arg(s.after),
             QStringLiteral("before_suspend=%1").arg(s.beforeSuspend ? "on" : "off"),
             QStringLiteral("skip_fullscreen=%1").arg(s.skipFullscreen ? "on" : "off"),
             QStringLiteral("skip_audio=%1").arg(s.skipAudio ? "on" : "off"),
             QStringLiteral("display_off=%1").arg(s.displayOff)}))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(lockConf))));
    if (s.beforeSuspend)
        takeSleepInhibitor();
    else
        releaseSleepInhibitor();
    m_fired = false;
    tick();
}

/* Polls the idle time, but only as often as needed: it sleeps until the
 * earliest moment the limit could be reached, then checks again. */
void ScreenLock::tick()
{
    m_idle.stop();
#ifdef FANSHELL_QS_HAVE_WAYLAND
    if (Surface::wayland()) {
        tickWayland();
        return;
    }
#endif
    xcb_connection_t *c = conn();
    if (!c || m_settings.after <= 0)
        return;
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_screensaver_id);
    if (!ext || !ext->present)
        return;
    xcb_screensaver_query_info_reply_t *info = xcb_screensaver_query_info_reply(
        c, xcb_screensaver_query_info(c, rootWindow(c)), nullptr);
    if (!info) {
        m_idle.start(30000);
        return;
    }
    const qint64 idle = info->ms_since_user_input;
    free(info);

    /* Input since the last look: a new stretch of idle. */
    if (idle < m_lastIdle)
        m_fired = false;
    m_lastIdle = idle;

    const qint64 limit = qint64(m_settings.after) * 1000;
    if (idle < limit) {
        m_idle.start(int(std::clamp<qint64>(limit - idle + 50, 1000, 60000)));
        return;
    }
    /* Just started, and the sound server has not said what is playing yet:
     * wait for it rather than lock over a video. Not for long, in case
     * there is no sound server. */
    if (m_settings.skipAudio && !PulseBackend::instance()->playingKnown
        && m_age.elapsed() < 10000) {
        m_idle.start(1000);
        return;
    }
    if (!m_fired && !m_locked && !(m_settings.skipFullscreen && fullscreenFocused(c))
        && pausedBy().isEmpty()) {
        m_fired = true;
        lockNow();
    }
    /* Past the limit: watch for the input that starts the next stretch, or
     * for the fullscreen window going away. */
    m_idle.start(5000);
}

#ifdef FANSHELL_QS_HAVE_WAYLAND
/* Wayland: told when idle starts and ends, rather than polling. The
 * compositor holds `idled` back while a program inhibits idling through
 * Wayland (a fullscreen video); the D-Bus requests, audio and the
 * fullscreen check are ours, so while idle they are looked at every few
 * seconds, as on X11. */
void ScreenLock::tickWayland()
{
    const int ms = m_settings.after * 1000;
    if (ms <= 0 || !Wl::IdleWatch::supported()) {
        delete m_idleWatch;
        m_idleWatch = nullptr;
        return;
    }
    if (!m_idleWatch || m_idleMs != ms) {
        delete m_idleWatch;
        m_idleMs = ms;
        m_idleWatch = new Wl::IdleWatch(ms, false, this);
        connect(m_idleWatch, &Wl::IdleWatch::idled, this, &ScreenLock::tick);
        connect(m_idleWatch, &Wl::IdleWatch::resumed, this, [this]() {
            m_fired = false;
            m_idle.stop();
        });
    }
    if (!m_idleWatch->idle())
        return;
    if (m_settings.skipAudio && !PulseBackend::instance()->playingKnown
        && m_age.elapsed() < 10000) {
        m_idle.start(1000);
        return;
    }
    if (!m_fired && !m_locked
        && !(m_settings.skipFullscreen && WmState::instance()->focusedFullscreen())
        && pausedBy().isEmpty()) {
        m_fired = true;
        lockNow();
    }
    m_idle.start(5000);
}
#endif

QStringList ScreenLock::pausedBy() const
{
    QStringList out = IdleInhibitor::instance()->reasons();
    const QStringList audio = PulseBackend::instance()->playing;
    if (m_settings.skipAudio && !audio.isEmpty())
        out << QStringLiteral("audio from ") + audio.join(QStringLiteral(", "));
    return out;
}

void ScreenLock::lockNow(std::function<void()> done)
{
    if (!m_locker) {
        emit aboutToLock();
        m_locker = new QProcess(this);
        m_locker->setProgram(QStringLiteral("fanshell-qs-lock"));
        auto finish = [this]() {
            if (!m_locker)
                return;
            m_locker->deleteLater();
            m_locker = nullptr;
            if (LockScreen::isLocked())
                setLocked(true);
            emit lockerDone();
        };
        connect(m_locker, &QProcess::finished, this, finish);
        connect(m_locker, &QProcess::errorOccurred, this,
                [finish](QProcess::ProcessError e) {
                    if (e == QProcess::FailedToStart)
                        finish();
                });
        m_locker->start();
    }
    if (done) {
        /* The script returns once the screen is locked; don't hold anything
         * up for longer than logind would wait anyway. */
        auto once = std::make_shared<bool>(false);
        auto run = [once, done]() {
            if (!*once) {
                *once = true;
                done();
            }
        };
        connect(this, &ScreenLock::lockerDone, this, run,
                Qt::SingleShotConnection);
        QTimer::singleShot(4000, this, run);
    }
}

void ScreenLock::prepareForSleep(bool sleeping)
{
    if (!sleeping) {
        m_fired = false;
        if (m_settings.beforeSuspend)
            takeSleepInhibitor();
        tick();
        return;
    }
    if (m_settings.beforeSuspend && m_sleepFd >= 0)
        lockNow([this]() { releaseSleepInhibitor(); });
    else
        releaseSleepInhibitor();
}

void ScreenLock::takeSleepInhibitor()
{
    if (m_sleepFd >= 0)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("Inhibit"));
    call << QStringLiteral("sleep") << QStringLiteral("fanshell-qs")
         << QStringLiteral("Lock the screen before sleeping")
         << QStringLiteral("delay");
    const QDBusReply<QDBusUnixFileDescriptor> reply =
        QDBusConnection::systemBus().call(call);
    if (!reply.isValid()) {
        qWarning("fanshell-qs: no logind sleep inhibitor (%s); the screen "
                 "will not lock before sleep",
                 qPrintable(reply.error().message()));
        return;
    }
    /* The descriptor object closes its own copy. */
    m_sleepFd = ::dup(reply.value().fileDescriptor());
}

void ScreenLock::releaseSleepInhibitor()
{
    if (m_sleepFd < 0)
        return;
    ::close(m_sleepFd);
    m_sleepFd = -1;
}

void ScreenLock::openWindow()
{
    if (!m_win)
        m_win = new ScreenLockWindow();
    m_win->open();
}

void ScreenLock::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void ScreenLock::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}
