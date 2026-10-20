#include "mouse.h"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "overlay.h"
#include "paths.h"
#include "widgets.h"

namespace {

constexpr auto mouseConf = "mouse.conf";
constexpr auto defaultsConf = "mouse.defaults.conf";

enum class Kind { Float, Bool, OneHot };

/* A setting, the libinput property behind it, and for a one-hot property
 * (one flag per choice) the choices by position and which of them the
 * popup offers. */
struct Prop {
    const char *key;
    const char *xprop;
    Kind kind;
    const char *availableProp = nullptr;
    QStringList values = {};
    QStringList offered = {};
};

const QVector<Prop> &props()
{
    static const QVector<Prop> p{
        {"speed", "libinput Accel Speed", Kind::Float},
        {"accel", "libinput Accel Profile Enabled", Kind::OneHot,
         "libinput Accel Profiles Available",
         {QStringLiteral("adaptive"), QStringLiteral("flat"), QStringLiteral("custom")},
         {QStringLiteral("adaptive"), QStringLiteral("flat")}},
        {"natural", "libinput Natural Scrolling Enabled", Kind::Bool},
        {"left_handed", "libinput Left Handed Enabled", Kind::Bool},
        {"tap", "libinput Tapping Enabled", Kind::Bool},
        {"dwt", "libinput Disable While Typing Enabled", Kind::Bool},
        {"scroll", "libinput Scroll Method Enabled", Kind::OneHot,
         "libinput Scroll Methods Available",
         {QStringLiteral("two-finger"), QStringLiteral("edge"), QStringLiteral("button")},
         {QStringLiteral("two-finger"), QStringLiteral("edge")}},
    };
    return p;
}

const Prop *findProp(const QString &key)
{
    for (const Prop &p : props())
        if (key == QLatin1String(p.key))
            return &p;
    return nullptr;
}

xcb_connection_t *connection()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

/* XCB_ATOM_NONE when the server has never heard of the name: no device
 * has that property. */
xcb_atom_t atom(const QByteArray &name)
{
    static QHash<QByteArray, xcb_atom_t> cache;
    const auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return *it;
    xcb_connection_t *c = connection();
    if (!c)
        return XCB_ATOM_NONE;
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 1, name.size(), name.constData()), nullptr);
    const xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    if (a != XCB_ATOM_NONE)
        cache.insert(name, a);
    return a;
}

/* A device property as it is on the wire; 32-bit items hold a float's
 * bits for FLOAT properties. */
struct Raw {
    xcb_atom_t type = XCB_ATOM_NONE;
    uint8_t format = 0;
    QVector<uint32_t> items;
};

Raw readProp(int device, xcb_atom_t prop)
{
    Raw raw;
    xcb_connection_t *c = connection();
    if (!c || prop == XCB_ATOM_NONE)
        return raw;
    xcb_input_xi_get_property_reply_t *r = xcb_input_xi_get_property_reply(
        c, xcb_input_xi_get_property(c, device, 0, prop, XCB_ATOM_ANY, 0, 64),
        nullptr);
    if (!r)
        return raw;
    if (r->type != XCB_ATOM_NONE) {
        raw.type = r->type;
        raw.format = r->format;
        const void *data = xcb_input_xi_get_property_items(r);
        for (uint32_t i = 0; i < r->num_items; i++) {
            if (r->format == 8)
                raw.items << static_cast<const uint8_t *>(data)[i];
            else if (r->format == 16)
                raw.items << static_cast<const uint16_t *>(data)[i];
            else
                raw.items << static_cast<const uint32_t *>(data)[i];
        }
    }
    free(r);
    return raw;
}

void writeProp(int device, xcb_atom_t prop, const Raw &raw)
{
    xcb_connection_t *c = connection();
    if (!c || prop == XCB_ATOM_NONE || raw.type == XCB_ATOM_NONE || raw.items.isEmpty())
        return;
    QByteArray buf;
    for (uint32_t v : raw.items) {
        if (raw.format == 8) {
            buf.append(char(v));
        } else if (raw.format == 16) {
            const uint16_t s = v;
            buf.append(reinterpret_cast<const char *>(&s), 2);
        } else {
            buf.append(reinterpret_cast<const char *>(&v), 4);
        }
    }
    xcb_input_xi_change_property(c, device, XCB_PROP_MODE_REPLACE, raw.format,
                                 prop, raw.type, raw.items.size(), buf.constData());
}

bool deviceHas(int device, xcb_atom_t prop)
{
    xcb_connection_t *c = connection();
    if (!c || prop == XCB_ATOM_NONE)
        return false;
    xcb_input_xi_list_properties_reply_t *r = xcb_input_xi_list_properties_reply(
        c, xcb_input_xi_list_properties(c, device), nullptr);
    if (!r)
        return false;
    const xcb_atom_t *atoms = xcb_input_xi_list_properties_properties(r);
    bool found = false;
    for (int i = 0; i < xcb_input_xi_list_properties_properties_length(r); i++)
        found = found || atoms[i] == prop;
    free(r);
    return found;
}

float toFloat(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

uint32_t fromFloat(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

/* Raw for `value` in the shape of what the device has now. */
bool encode(const Prop &p, const QString &value, Raw &raw)
{
    if (raw.items.isEmpty())
        return false;
    switch (p.kind) {
    case Kind::Float: {
        bool ok = false;
        const double v = value.toDouble(&ok);
        if (!ok)
            return false;
        raw.items[0] = fromFloat(float(qBound(-1.0, v, 1.0)));
        return true;
    }
    case Kind::Bool:
        if (value != QLatin1String("on") && value != QLatin1String("off"))
            return false;
        raw.items[0] = value == QLatin1String("on");
        return true;
    case Kind::OneHot: {
        const int i = int(p.values.indexOf(value));
        if (i < 0 || i >= raw.items.size())
            return false;
        for (uint32_t &v : raw.items)
            v = 0;
        raw.items[i] = 1;
        return true;
    }
    }
    return false;
}

QString decode(const Prop &p, const Raw &raw)
{
    if (raw.items.isEmpty())
        return QString();
    switch (p.kind) {
    case Kind::Float: {
        const double v = qRound(toFloat(raw.items[0]) * 100) / 100.0;
        return QString::number(v == 0 ? 0.0 : v, 'f', 2);
    }
    case Kind::Bool:
        return raw.items[0] ? QStringLiteral("on") : QStringLiteral("off");
    case Kind::OneHot:
        for (int i = 0; i < raw.items.size() && i < p.values.size(); i++)
            if (raw.items[i])
                return p.values[i];
        return QString();
    }
    return QString();
}

/* Pointer devices being added, removed, enabled or disabled (XInput 2
 * hierarchy events, which Qt's xcb backend already selects on the root
 * window). */
class MouseFilter : public QAbstractNativeEventFilter {
public:
    explicit MouseFilter(int xinputOpcode) : m_xinput(xinputOpcode) {}

    bool nativeEventFilter(const QByteArray &type, void *message,
                           qintptr *) override
    {
        if (type != "xcb_generic_event_t")
            return false;
        const auto *ev = static_cast<const xcb_generic_event_t *>(message);
        if ((ev->response_type & 0x7f) != XCB_GE_GENERIC)
            return false;
        const auto *g = static_cast<const xcb_ge_generic_event_t *>(message);
        if (g->extension != m_xinput || g->event_type != XCB_INPUT_HIERARCHY)
            return false;
        const auto *h = static_cast<const xcb_input_hierarchy_event_t *>(message);
        const xcb_input_hierarchy_info_t *info = xcb_input_hierarchy_infos(h);
        for (int i = 0; i < xcb_input_hierarchy_infos_length(h); i++)
            if (info[i].type == XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER && info[i].flags) {
                MouseSettings::instance()->pointerAdded();
                break;
            }
        return false;
    }

private:
    int m_xinput;
};

QString choiceLabel(const QString &value)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("adaptive"), QStringLiteral("Adaptive")},
        {QStringLiteral("flat"), QStringLiteral("Flat")},
        {QStringLiteral("two-finger"), QStringLiteral("Two fingers")},
        {QStringLiteral("edge"), QStringLiteral("Edge")},
    };
    return labels.value(value, value);
}

} /* namespace */

class MouseWindow : public CenteredOverlay {
public:
    MouseWindow()
        : CenteredOverlay(QStringLiteral("fanshell-mouse"),
                          QStringLiteral("fanshell-qs-mouse"), 460)
    {
        auto *lay = new QVBoxLayout(box());
        lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                Theme::popupMargin, Theme::popupMargin);
        lay->setSpacing(Theme::popupSpacing);

        auto *title = new TextItem(box());
        title->setPixelSize(Theme::bodyFontSize);
        title->setBold(true);
        title->setColor(Theme::textStrong);
        title->setText(QStringLiteral("Mouse & touchpad"));
        lay->addWidget(title);

        m_empty = new TextItem(box());
        m_empty->setPixelSize(Theme::smallFontSize);
        m_empty->setColor(Theme::textMuted);
        m_empty->setText(QStringLiteral("No mouse or touchpad on the libinput driver."));
        lay->addWidget(m_empty);

        auto *rows = new QVBoxLayout();
        rows->setSpacing(Theme::rowSpacing);
        lay->addLayout(rows);
        /* One row each, hidden when the device lacks the setting. */
        auto addRow = [this, rows](const QString &name, QWidget *field) {
            auto *row = new QWidget(box());
            auto *rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(Theme::sectionSpacing);
            auto *label = new TextItem(row);
            label->setPixelSize(Theme::smallFontSize);
            label->setColor(Theme::textMuted);
            label->setText(name);
            label->setFixedWidth(150);
            field->setParent(row);
            rl->addWidget(label);
            rl->addWidget(field, 1);
            rows->addWidget(row);
            return row;
        };
        auto toggle = [this](ShellButton **button) {
            auto *w = new QWidget(box());
            auto *l = new QHBoxLayout(w);
            l->setContentsMargins(0, 0, 0, 0);
            *button = new ShellButton(QStringLiteral("Off"), w);
            l->addWidget(*button);
            l->addStretch(1);
            return w;
        };

        m_device = new Selector(choiceList(), box());
        m_deviceRow = addRow(QStringLiteral("Device"), m_device);

        auto *speed = new QWidget(box());
        auto *sl = new QHBoxLayout(speed);
        sl->setContentsMargins(0, 0, 0, 0);
        sl->setSpacing(Theme::rowSpacing);
        m_speed = new ValueSlider(speed);
        m_speed->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_speedText = new TextItem(speed);
        m_speedText->setPixelSize(Theme::smallFontSize);
        m_speedText->setHAlign(Qt::AlignRight);
        m_speedText->setFixedWidth(44);
        sl->addWidget(m_speed, 1, Qt::AlignVCenter);
        sl->addWidget(m_speedText, 0, Qt::AlignVCenter);
        m_rows.insert(QStringLiteral("speed"), addRow(QStringLiteral("Speed"), speed));

        m_accel = new Selector(choiceList(), box());
        m_rows.insert(QStringLiteral("accel"), addRow(QStringLiteral("Acceleration"), m_accel));
        m_rows.insert(QStringLiteral("natural"),
                      addRow(QStringLiteral("Natural scrolling"), toggle(&m_natural)));
        m_rows.insert(QStringLiteral("left_handed"),
                      addRow(QStringLiteral("Left-handed"), toggle(&m_leftHanded)));
        m_rows.insert(QStringLiteral("tap"),
                      addRow(QStringLiteral("Tap to click"), toggle(&m_tap)));
        m_rows.insert(QStringLiteral("dwt"),
                      addRow(QStringLiteral("Disable while typing"), toggle(&m_dwt)));
        m_scroll = new Selector(choiceList(), box());
        m_rows.insert(QStringLiteral("scroll"), addRow(QStringLiteral("Scrolling"), m_scroll));

        m_note = new QWidget(box());
        auto *nl = new QVBoxLayout(m_note);
        nl->setContentsMargins(0, 0, 0, 0);
        nl->setSpacing(2);
        for (const char *line : {"Flat is no acceleration: the same hand movement always",
                                 "moves the pointer the same distance.",
                                 "Settings follow the device by name."}) {
            auto *t = new TextItem(m_note);
            t->setPixelSize(Theme::tinyFontSize);
            t->setColor(Theme::textMuted);
            t->setText(QString::fromUtf8(line));
            nl->addWidget(t);
        }
        lay->addWidget(m_note);

        lay->addWidget(new HLine(box()));
        auto *footer = new QWidget(box());
        auto *fl = new QHBoxLayout(footer);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(Theme::rowSpacing);
        m_reset = new ShellButton(QStringLiteral("Reset device"), footer);
        auto *close = new ShellButton(QStringLiteral("Close"), footer);
        fl->addWidget(m_reset);
        fl->addStretch(1);
        fl->addWidget(close);
        lay->addWidget(footer);

        m_device->chosen = [this](int i) {
            if (i >= 0 && i < m_devices.size()) {
                m_current = m_devices[i].name;
                sync();
            }
        };
        connect(m_speed, &ValueSlider::moved, this, [this](qreal v) {
            /* -1..1 in steps of 0.05. */
            const double speed = qRound((v * 2 - 1) * 20) / 20.0;
            change(QStringLiteral("speed"), QString::number(speed == 0 ? 0.0 : speed, 'f', 2));
        });
        auto pick = [this](Selector *sel, const QString &key) {
            sel->chosen = [this, key](int i) {
                const MouseSettings::Device *d = device();
                if (!d)
                    return;
                const QStringList v = MouseSettings::instance()->available(*d, key);
                if (i >= 0 && i < v.size())
                    change(key, v[i]);
            };
        };
        pick(m_accel, QStringLiteral("accel"));
        pick(m_scroll, QStringLiteral("scroll"));
        auto flip = [this](ShellButton *b, const QString &key) {
            connect(b, &ShellButton::activated, this, [this, key]() {
                const MouseSettings::Device *d = device();
                if (d)
                    change(key, MouseSettings::instance()->current(*d, key)
                                        == QLatin1String("on")
                                    ? QStringLiteral("off")
                                    : QStringLiteral("on"));
            });
        };
        flip(m_natural, QStringLiteral("natural"));
        flip(m_leftHanded, QStringLiteral("left_handed"));
        flip(m_tap, QStringLiteral("tap"));
        flip(m_dwt, QStringLiteral("dwt"));
        connect(m_reset, &ShellButton::activated, this, [this]() {
            if (const MouseSettings::Device *d = device()) {
                MouseSettings::instance()->reset(*d);
                sync();
            }
        });
        connect(close, &ShellButton::activated, this, [this]() { dismiss(); });
    }

    void open()
    {
        refresh();
        present();
    }

    /* The device list again, after a device came or went. */
    void refresh()
    {
        m_devices = MouseSettings::instance()->devices();
        sync();
    }

private:
    const MouseSettings::Device *device() const
    {
        for (const MouseSettings::Device &d : m_devices)
            if (d.name == m_current)
                return &d;
        return nullptr;
    }

    void change(const QString &key, const QString &value)
    {
        if (const MouseSettings::Device *d = device()) {
            MouseSettings::instance()->set(*d, key, value);
            sync();
        }
    }

    void sync()
    {
        if (!device() && !m_devices.isEmpty())
            m_current = m_devices.first().name;
        const MouseSettings::Device *d = device();
        m_empty->setVisible(!d);
        m_deviceRow->setVisible(d);
        m_note->setVisible(d);
        m_reset->setVisible(d);
        QStringList names;
        int index = -1;
        for (int i = 0; i < m_devices.size(); i++) {
            names << m_devices[i].name
                         + (m_devices[i].touchpad ? QStringLiteral(" (touchpad)") : QString());
            if (m_devices[i].name == m_current)
                index = i;
        }
        m_device->setChoices(names, index);

        MouseSettings *m = MouseSettings::instance();
        for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it) {
            bool shown = d && m->has(*d, it.key());
            /* A choice of one is no choice. */
            if (shown && (it.key() == QLatin1String("accel")
                          || it.key() == QLatin1String("scroll")))
                shown = m->available(*d, it.key()).size() > 1;
            it.value()->setVisible(shown);
        }
        if (!d) {
            if (isVisible())
                layoutBox();
            return;
        }

        const double speed = m->current(*d, QStringLiteral("speed")).toDouble();
        m_speed->setValue((speed + 1) / 2);
        m_speedText->setText(speed == 0 ? QStringLiteral("0.00")
                                        : QString::asprintf("%+.2f", speed));
        auto choices = [m, d](Selector *sel, const QString &key) {
            const QStringList v = m->available(*d, key);
            QStringList labels;
            for (const QString &s : v)
                labels << choiceLabel(s);
            sel->setChoices(labels, int(v.indexOf(m->current(*d, key))));
        };
        choices(m_accel, QStringLiteral("accel"));
        choices(m_scroll, QStringLiteral("scroll"));
        auto onOff = [m, d](ShellButton *b, const QString &key) {
            const bool on = m->current(*d, key) == QLatin1String("on");
            b->setLabel(on ? QStringLiteral("On") : QStringLiteral("Off"));
            b->setActive(on);
        };
        onOff(m_natural, QStringLiteral("natural"));
        onOff(m_leftHanded, QStringLiteral("left_handed"));
        onOff(m_tap, QStringLiteral("tap"));
        onOff(m_dwt, QStringLiteral("dwt"));
        if (isVisible())
            layoutBox();
    }

    QVector<MouseSettings::Device> m_devices;
    QString m_current;
    TextItem *m_empty;
    QWidget *m_deviceRow;
    Selector *m_device;
    QHash<QString, QWidget *> m_rows;
    ValueSlider *m_speed;
    TextItem *m_speedText;
    Selector *m_accel;
    Selector *m_scroll;
    ShellButton *m_natural;
    ShellButton *m_leftHanded;
    ShellButton *m_tap;
    ShellButton *m_dwt;
    QWidget *m_note;
    ShellButton *m_reset;
};

MouseSettings *MouseSettings::instance()
{
    static MouseSettings *s = new MouseSettings();
    return s;
}

MouseSettings::MouseSettings(QObject *parent) : QObject(parent)
{
    load();
    m_hotplug.setSingleShot(true);
    m_hotplug.setInterval(500);
    connect(&m_hotplug, &QTimer::timeout, this, [this]() {
        applyAll();
        if (m_win && m_win->isVisible())
            m_win->refresh();
    });
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(400);
    connect(&m_saveTimer, &QTimer::timeout, this, &MouseSettings::save);

    if (xcb_connection_t *c = connection()) {
        const xcb_query_extension_reply_t *xi =
            xcb_get_extension_data(c, &xcb_input_id);
        if (xi && xi->present)
            qApp->installNativeEventFilter(new MouseFilter(xi->major_opcode));
    }
    applyAll();
}

void MouseSettings::load()
{
    auto parse = [](const QHash<QString, QString> &conf, auto add) {
        for (auto it = conf.constBegin(); it != conf.constEnd(); ++it) {
            const int colon = it.key().indexOf(QLatin1Char(':'));
            if (colon <= 0 || !findProp(it.key().left(colon)))
                continue;
            add(it.key().left(colon), it.key().mid(colon + 1).trimmed(), it.value());
        }
    };
    parse(Paths::readConf(QLatin1String(mouseConf)),
          [this](const QString &key, const QString &name, const QString &value) {
              m_saved[name].insert(key, value);
          });
    parse(Paths::readConf(QLatin1String(defaultsConf)),
          [this](const QString &key, const QString &name, const QString &value) {
              m_defaults << Rule{name.toLower(), key, value};
          });
}

void MouseSettings::save()
{
    m_saveTimer.stop();
    QStringList lines;
    for (auto d = m_saved.constBegin(); d != m_saved.constEnd(); ++d)
        for (auto s = d.value().constBegin(); s != d.value().constEnd(); ++s)
            lines << s.key() + QLatin1Char(':') + d.key() + QLatin1Char('=') + s.value();
    lines.sort();
    if (!Paths::writeConf(
            QLatin1String(mouseConf),
            QStringLiteral("Mouse and touchpad settings, set from the Mouse & "
                           "touchpad popup.\nSETTING:DEVICE NAME=VALUE; see "
                           "shell/mouse.h."),
            lines))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(mouseConf))));
}

QVector<MouseSettings::Device> MouseSettings::devices() const
{
    QVector<Device> out;
    xcb_connection_t *c = connection();
    const xcb_atom_t speed = atom("libinput Accel Speed");
    const xcb_atom_t tapping = atom("libinput Tapping Enabled");
    if (!c || speed == XCB_ATOM_NONE)
        return out;
    xcb_input_xi_query_device_reply_t *r = xcb_input_xi_query_device_reply(
        c, xcb_input_xi_query_device(c, XCB_INPUT_DEVICE_ALL), nullptr);
    if (!r)
        return out;
    for (xcb_input_xi_device_info_iterator_t it =
             xcb_input_xi_query_device_infos_iterator(r);
         it.rem; xcb_input_xi_device_info_next(&it)) {
        const xcb_input_xi_device_info_t *info = it.data;
        if (info->type != XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER || !info->enabled)
            continue;
        const QString name = QString::fromUtf8(xcb_input_xi_device_info_name(info),
                                               xcb_input_xi_device_info_name_length(info));
        /* Only what moves the pointer: a keyboard's or a headset's
         * media-key interface is a slave pointer too, with no speed. */
        if (name.contains(QLatin1String("XTEST")) || !deviceHas(info->deviceid, speed))
            continue;
        auto same = std::find_if(out.begin(), out.end(),
                                 [&name](const Device &d) { return d.name == name; });
        if (same == out.end()) {
            out << Device{name, {info->deviceid}, false};
            same = out.end() - 1;
        } else {
            same->ids << info->deviceid;
        }
        /* A touchpad can come with siblings of the same name (its pen or
         * its buttons) that lack the touchpad's settings: the popup reads
         * the first id, so the touchpad goes first. */
        if (!same->touchpad && deviceHas(info->deviceid, tapping)) {
            same->touchpad = true;
            same->ids.move(same->ids.size() - 1, 0);
        }
    }
    free(r);
    std::sort(out.begin(), out.end(), [](const Device &a, const Device &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

bool MouseSettings::has(const Device &d, const QString &key) const
{
    const Prop *p = findProp(key);
    return p && !d.ids.isEmpty() && deviceHas(d.ids.first(), atom(p->xprop));
}

QString MouseSettings::current(const Device &d, const QString &key) const
{
    const Prop *p = findProp(key);
    if (!p || d.ids.isEmpty())
        return QString();
    return decode(*p, readProp(d.ids.first(), atom(p->xprop)));
}

QStringList MouseSettings::available(const Device &d, const QString &key) const
{
    const Prop *p = findProp(key);
    if (!p || p->kind != Kind::OneHot || d.ids.isEmpty() || !p->availableProp)
        return {};
    const Raw raw = readProp(d.ids.first(), atom(p->availableProp));
    QStringList out;
    for (int i = 0; i < raw.items.size() && i < p->values.size(); i++)
        if (raw.items[i] && p->offered.contains(p->values[i]))
            out << p->values[i];
    return out;
}

void MouseSettings::set(const Device &d, const QString &key, const QString &value)
{
    const Prop *p = findProp(key);
    if (!p)
        return;
    const xcb_atom_t a = atom(p->xprop);
    for (int id : d.ids) {
        Raw raw = readProp(id, a);
        if (encode(*p, value, raw))
            writeProp(id, a, raw);
    }
    if (xcb_connection_t *c = connection())
        xcb_flush(c);
    m_saved[d.name].insert(key, value);
    m_saveTimer.start();
}

void MouseSettings::reset(const Device &d)
{
    for (const Prop &p : props()) {
        const xcb_atom_t a = atom(p.xprop);
        const xcb_atom_t def = atom(QByteArray(p.xprop) + " Default");
        for (int id : d.ids) {
            const Raw raw = readProp(id, def);
            if (!raw.items.isEmpty())
                writeProp(id, a, raw);
        }
    }
    m_saved.remove(d.name);
    save();
    apply(d);
}

QString MouseSettings::wanted(const QString &name, const QString &key) const
{
    const QString saved = m_saved.value(name).value(key);
    if (!saved.isEmpty())
        return saved;
    const QString lower = name.toLower();
    QString value;
    for (const Rule &r : m_defaults)
        if (r.key == key && lower.contains(r.pattern))
            value = r.value;
    return value;
}

void MouseSettings::apply(const Device &d)
{
    for (const Prop &p : props()) {
        const QString value = wanted(d.name, QLatin1String(p.key));
        if (value.isEmpty())
            continue;
        const xcb_atom_t a = atom(p.xprop);
        for (int id : d.ids) {
            Raw raw = readProp(id, a);
            if (encode(p, value, raw))
                writeProp(id, a, raw);
        }
    }
    if (xcb_connection_t *c = connection())
        xcb_flush(c);
}

void MouseSettings::applyAll()
{
    for (const Device &d : devices())
        apply(d);
}

void MouseSettings::pointerAdded()
{
    m_hotplug.start();
}

void MouseSettings::openWindow()
{
    if (!m_win)
        m_win = new MouseWindow();
    m_win->open();
}

void MouseSettings::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void MouseSettings::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}
