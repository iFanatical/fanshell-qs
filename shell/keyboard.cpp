#include "keyboard.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDateTime>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QProcess>
#include <QVBoxLayout>

#include <xcb/screensaver.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
/* xkb.h names a struct field `explicit`, which C++ reserves. */
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "hardwarecontrols.h"
#include "osd.h"
#include "overlay.h"
#include "paths.h"
#include "surface.h"
#include "widgets.h"
#include "wmstate.h"
#ifdef FANSHELL_QS_HAVE_WAYLAND
#include <QDir>
#include <QFile>

#include "hypripc.h"
#include "wlclient.h"
#endif

/* Last: Xlib's macros (None, Bool, Status, ...) collide with Qt's names. */
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XKBrules.h>
#include <X11/keysym.h>

namespace {

constexpr auto keyboardConf = "keyboard.conf";
/* On the root window once Num Lock has been set for this X session. */
constexpr auto appliedAtom = "_FANSHELL_QS_NUMLOCK_APPLIED";
/* Where setxkbmap finds the rules named in _XKB_RULES_NAMES ("evdev"). */
constexpr auto rulesDir = "/usr/share/X11/xkb/rules/";

/* Settings values, their labels, and the XKB option each one sets. */
struct Choice {
    const char *value;
    const char *label;
    const char *option;
};
const Choice capsChoices[] = {
    {"default", "Caps Lock", ""},
    {"escape", "Escape", "caps:escape"},
    {"ctrl", "Ctrl", "ctrl:nocaps"},
    {"swap", "Swap with Ctrl", "ctrl:swapcaps"},
    {"none", "Nothing", "caps:none"},
};
const Choice composeChoices[] = {
    {"none", "None", ""},
    {"ralt", "Right Alt", "compose:ralt"},
    {"menu", "Menu", "compose:menu"},
    {"rctrl", "Right Ctrl", "compose:rctrl"},
};
const QVector<int> delayChoices{150, 200, 250, 300, 400, 500, 660};
const QVector<int> rateChoices{15, 20, 25, 30, 35, 40, 50};
const QVector<int> backlightChoices{0, 10, 30, 60, 120, 300};

template <size_t N>
int choiceIndex(const Choice (&choices)[N], const QString &value)
{
    for (size_t i = 0; i < N; i++)
        if (value == QLatin1String(choices[i].value))
            return int(i);
    return 0;
}

/* The options this window owns; any others in the keymap are kept. */
bool managedOption(const QString &o)
{
    return o.startsWith(QLatin1String("caps:"))
           || o.startsWith(QLatin1String("compose:"))
           || o == QLatin1String("ctrl:nocaps")
           || o == QLatin1String("ctrl:swapcaps");
}

/* Presets, plus `current` when it is something else (hand-edited). */
QVector<int> withCurrent(QVector<int> v, int current)
{
    if (!v.contains(current)) {
        v << current;
        std::sort(v.begin(), v.end());
    }
    return v;
}

QString secondsLabel(int s)
{
    if (s <= 0)
        return QStringLiteral("Never");
    if (s % 60 == 0)
        return QStringLiteral("%1 min").arg(s / 60);
    return QStringLiteral("%1 s").arg(s);
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

Display *display()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->display() : nullptr;
}

xcb_connection_t *connection()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

/* The modifier Num_Lock is bound to (normally Mod2), or 0 without one. */
unsigned int numlockMask(Display *dpy)
{
    return XkbKeysymToModifiers(dpy, XK_Num_Lock);
}

bool setNumlock(bool on)
{
    Display *dpy = display();
    if (!dpy)
        return false;
    const unsigned int mask = numlockMask(dpy);
    if (!mask)
        return false;
    XkbLockModifiers(dpy, XkbUseCoreKbd, mask, on ? mask : 0);
    XFlush(dpy);
    return true;
}

/* Set the marker, and say whether it was already there. */
bool markApplied()
{
    Display *dpy = display();
    if (!dpy)
        return true;
    const Window root = DefaultRootWindow(dpy);
    const Atom atom = XInternAtom(dpy, appliedAtom, False);
    Atom type = 0;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char *data = nullptr;
    const bool present =
        XGetWindowProperty(dpy, root, atom, 0, 1, False, AnyPropertyType,
                           &type, &format, &items, &after, &data) == Success
        && type != 0;
    if (data)
        XFree(data);
    if (!present) {
        const unsigned char one = 1;
        XChangeProperty(dpy, root, atom, XA_CARDINAL, 8, PropModeReplace,
                        &one, 1);
        XFlush(dpy);
    }
    return present;
}

#ifdef FANSHELL_QS_HAVE_WAYLAND
/* Caps Lock and Num Lock as the keyboards' LEDs show them, which Hyprland
 * keeps in step with its keymap state; it reports no lock changes over IPC
 * and Wayland tells only the focused client. As locked modifiers: LockMask
 * for Caps Lock, Mod2Mask for Num Lock (their usual places). */
unsigned int ledLocks()
{
    unsigned int mods = 0;
    const QDir leds(QStringLiteral("/sys/class/leds"));
    for (const QString &name : leds.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const bool caps = name.endsWith(QLatin1String("::capslock"));
        const bool num = name.endsWith(QLatin1String("::numlock"));
        if (!caps && !num)
            continue;
        QFile f(leds.filePath(name) + QStringLiteral("/brightness"));
        if (!f.open(QIODevice::ReadOnly) || f.readAll().trimmed().toInt() <= 0)
            continue;
        mods |= caps ? LockMask : Mod2Mask;
    }
    return mods;
}
#endif

/* Milliseconds since the last keyboard or mouse input, or -1. */
qint64 idleMs()
{
    xcb_connection_t *c = connection();
    if (!c)
        return -1;
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(c, &xcb_screensaver_id);
    if (!ext || !ext->present)
        return -1;
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_screensaver_query_info_reply_t *info = xcb_screensaver_query_info_reply(
        c, xcb_screensaver_query_info(c, root), nullptr);
    if (!info)
        return -1;
    const qint64 idle = info->ms_since_user_input;
    free(info);
    return idle;
}

/* Lock changes (XKB StateNotify) and keyboards being added (XInput 2
 * hierarchy events). Qt's own xcb backend already selects both -- the
 * first to track modifiers, the second on the root window for its device
 * list -- so this only has to look. */
class KeyboardFilter : public QAbstractNativeEventFilter {
public:
    KeyboardFilter(int xkbEvent, int xinputOpcode)
        : m_xkb(xkbEvent), m_xinput(xinputOpcode)
    {
    }

    bool nativeEventFilter(const QByteArray &type, void *message,
                           qintptr *) override
    {
        if (type != "xcb_generic_event_t")
            return false;
        const auto *ev = static_cast<const xcb_generic_event_t *>(message);
        const uint8_t t = ev->response_type & 0x7f;
        if (m_xkb >= 0 && t == m_xkb) {
            const auto *s = static_cast<const xcb_xkb_state_notify_event_t *>(message);
            if (s->xkbType == XCB_XKB_STATE_NOTIFY
                && (s->changed & XCB_XKB_STATE_PART_MODIFIER_LOCK))
                KeyboardSettings::instance()->lockStateChanged(s->lockedMods);
        } else if (m_xinput >= 0 && t == XCB_GE_GENERIC) {
            const auto *g = static_cast<const xcb_ge_generic_event_t *>(message);
            if (g->extension != m_xinput || g->event_type != XCB_INPUT_HIERARCHY)
                return false;
            const auto *h = static_cast<const xcb_input_hierarchy_event_t *>(message);
            constexpr uint32_t added = XCB_INPUT_HIERARCHY_MASK_SLAVE_ADDED
                                       | XCB_INPUT_HIERARCHY_MASK_SLAVE_ATTACHED
                                       | XCB_INPUT_HIERARCHY_MASK_DEVICE_ENABLED;
            if (!(h->flags & added))
                return false;
            const xcb_input_hierarchy_info_t *info = xcb_input_hierarchy_infos(h);
            for (int i = 0; i < xcb_input_hierarchy_infos_length(h); i++)
                if (info[i].type == XCB_INPUT_DEVICE_TYPE_SLAVE_KEYBOARD
                    && (info[i].flags & added)) {
                    KeyboardSettings::instance()->keyboardAdded();
                    break;
                }
        }
        return false;
    }

private:
    int m_xkb;
    int m_xinput;
};

} /* namespace */

class KeyboardWindow : public CenteredOverlay {
public:
    KeyboardWindow()
        : CenteredOverlay(QStringLiteral("fanshell-keyboard"),
                          QStringLiteral("fanshell-qs-keyboard"), 440)
    {
        auto *lay = new QVBoxLayout(box());
        lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                Theme::popupMargin, Theme::popupMargin);
        lay->setSpacing(Theme::popupSpacing);

        auto *title = new TextItem(box());
        title->setPixelSize(Theme::bodyFontSize);
        title->setBold(true);
        title->setColor(Theme::textStrong);
        title->setText(QStringLiteral("Keyboard"));
        lay->addWidget(title);

        auto *grid = new QGridLayout();
        grid->setHorizontalSpacing(Theme::sectionSpacing);
        grid->setVerticalSpacing(Theme::rowSpacing);
        grid->setColumnStretch(1, 1);
        int row = 0;
        auto addRow = [this, grid, &row](const QString &name, QWidget *field) {
            auto *label = new TextItem(box());
            label->setPixelSize(Theme::smallFontSize);
            label->setColor(Theme::textMuted);
            label->setText(name);
            grid->addWidget(label, row, 0);
            grid->addWidget(field, row, 1);
            row++;
        };
        auto toggle = [this](ShellButton **button) {
            *button = new ShellButton(QStringLiteral("Off"), box());
            auto *w = new QWidget(box());
            auto *l = new QHBoxLayout(w);
            l->setContentsMargins(0, 0, 0, 0);
            l->addWidget(*button);
            l->addStretch(1);
            return w;
        };

        m_delay = new Selector(choiceList(), box());
        addRow(QStringLiteral("Repeat delay"), m_delay);
        m_rate = new Selector(choiceList(), box());
        addRow(QStringLiteral("Repeat rate"), m_rate);
        m_caps = new Selector(choiceList(), box());
        addRow(QStringLiteral("Caps Lock key"), m_caps);
        m_compose = new Selector(choiceList(), box());
        addRow(QStringLiteral("Compose key"), m_compose);
        addRow(QStringLiteral("Num Lock at login"), toggle(&m_numlock));
        addRow(QStringLiteral("Show lock changes"), toggle(&m_lockOsd));
        if (HardwareControls::instance()->hasKeyboardBacklight()) {
            m_backlight = new Selector(choiceList(), box());
            addRow(QStringLiteral("Backlight off after"), m_backlight);
        }
        lay->addLayout(grid);

        auto *note = new QWidget(box());
        auto *nl = new QVBoxLayout(note);
        nl->setContentsMargins(0, 0, 0, 0);
        nl->setSpacing(2);
        QStringList lines{
            QStringLiteral("Compose, then two keys, types one: ' e is é, o \" is ö."),
            QStringLiteral("Show lock changes pops up Caps Lock and Num Lock.")};
        if (m_backlight)
            lines << QStringLiteral("The backlight comes back at the next key or mouse move.");
        for (const QString &line : lines) {
            auto *t = new TextItem(note);
            t->setPixelSize(Theme::tinyFontSize);
            t->setColor(Theme::textMuted);
            t->setText(line);
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

        auto edit = [this](auto change) {
            KeyboardSettings::Settings s = KeyboardSettings::instance()->settings();
            change(s);
            KeyboardSettings::instance()->set(s);
            sync();
        };
        m_delay->chosen = [this, edit](int i) {
            const QVector<int> v = withCurrent(delayChoices, current().repeatDelay);
            if (i >= 0 && i < v.size())
                edit([&](KeyboardSettings::Settings &s) { s.repeatDelay = v[i]; });
        };
        m_rate->chosen = [this, edit](int i) {
            const QVector<int> v = withCurrent(rateChoices, current().repeatRate);
            if (i >= 0 && i < v.size())
                edit([&](KeyboardSettings::Settings &s) { s.repeatRate = v[i]; });
        };
        m_caps->chosen = [edit](int i) {
            if (i >= 0 && i < int(std::size(capsChoices)))
                edit([&](KeyboardSettings::Settings &s) {
                    s.caps = QLatin1String(capsChoices[i].value);
                });
        };
        m_compose->chosen = [edit](int i) {
            if (i >= 0 && i < int(std::size(composeChoices)))
                edit([&](KeyboardSettings::Settings &s) {
                    s.compose = QLatin1String(composeChoices[i].value);
                });
        };
        if (m_backlight)
            m_backlight->chosen = [this, edit](int i) {
                const QVector<int> v =
                    withCurrent(backlightChoices, current().backlightIdle);
                if (i >= 0 && i < v.size())
                    edit([&](KeyboardSettings::Settings &s) { s.backlightIdle = v[i]; });
            };
        connect(m_numlock, &ShellButton::activated, this, [edit]() {
            edit([](KeyboardSettings::Settings &s) { s.numlock = !s.numlock; });
        });
        connect(m_lockOsd, &ShellButton::activated, this, [edit]() {
            edit([](KeyboardSettings::Settings &s) { s.lockOsd = !s.lockOsd; });
        });
        connect(close, &ShellButton::activated, this, [this]() { dismiss(); });
    }

    void open()
    {
        sync();
        present();
    }

private:
    static KeyboardSettings::Settings current()
    {
        return KeyboardSettings::instance()->settings();
    }

    void sync()
    {
        const KeyboardSettings::Settings s = current();
        auto numbers = [](Selector *sel, const QVector<int> &v, int value,
                          auto label) {
            QStringList labels;
            for (int n : v)
                labels << label(n);
            sel->setChoices(labels, int(v.indexOf(value)));
        };
        numbers(m_delay, withCurrent(delayChoices, s.repeatDelay), s.repeatDelay,
                [](int ms) { return QStringLiteral("%1 ms").arg(ms); });
        numbers(m_rate, withCurrent(rateChoices, s.repeatRate), s.repeatRate,
                [](int n) { return QStringLiteral("%1 per second").arg(n); });
        if (m_backlight)
            numbers(m_backlight, withCurrent(backlightChoices, s.backlightIdle),
                    s.backlightIdle, secondsLabel);

        QStringList caps, compose;
        for (const Choice &c : capsChoices)
            caps << QString::fromUtf8(c.label);
        for (const Choice &c : composeChoices)
            compose << QString::fromUtf8(c.label);
        m_caps->setChoices(caps, choiceIndex(capsChoices, s.caps));
        m_compose->setChoices(compose, choiceIndex(composeChoices, s.compose));

        auto onOff = [](ShellButton *b, bool on) {
            b->setLabel(on ? QStringLiteral("On") : QStringLiteral("Off"));
            b->setActive(on);
        };
        onOff(m_numlock, s.numlock);
        onOff(m_lockOsd, s.lockOsd);
    }

    Selector *m_delay;
    Selector *m_rate;
    Selector *m_caps;
    Selector *m_compose;
    Selector *m_backlight = nullptr;
    ShellButton *m_numlock;
    ShellButton *m_lockOsd;
};

KeyboardSettings *KeyboardSettings::instance()
{
    static KeyboardSettings *s = new KeyboardSettings();
    return s;
}

KeyboardSettings::KeyboardSettings(QObject *parent) : QObject(parent)
{
    const QHash<QString, QString> conf =
        Paths::readConf(QLatin1String(keyboardConf));
    auto number = [&conf](const char *key, int fallback, int lo, int hi) {
        bool ok = false;
        const int v = conf.value(QLatin1String(key)).toInt(&ok);
        return ok && v >= lo && v <= hi ? v : fallback;
    };
    Settings &s = m_settings;
    if (conf.contains(QStringLiteral("numlock")))
        s.numlock = conf.value(QStringLiteral("numlock")) != QLatin1String("off");
    if (conf.contains(QStringLiteral("lock_osd")))
        s.lockOsd = conf.value(QStringLiteral("lock_osd")) != QLatin1String("off");
    s.repeatDelay = number("repeat_delay", s.repeatDelay, 100, 1000);
    s.repeatRate = number("repeat_rate", s.repeatRate, 5, 100);
    s.backlightIdle = number("backlight_idle", s.backlightIdle, 0, 3600);
    s.caps = QLatin1String(
        capsChoices[choiceIndex(capsChoices, conf.value(QStringLiteral("caps")))].value);
    s.compose = QLatin1String(composeChoices[choiceIndex(
        composeChoices, conf.value(QStringLiteral("compose")))].value);

    m_hotplug.setSingleShot(true);
    m_hotplug.setInterval(500);
    connect(&m_hotplug, &QTimer::timeout, this, [this]() {
        applyOptions(true);
        applyRepeat();
    });

    m_backlightTimer.setSingleShot(true);
    connect(&m_backlightTimer, &QTimer::timeout, this,
            &KeyboardSettings::backlightTick);
    HardwareControls *hw = HardwareControls::instance();
    /* A level set while the light is off for idle -- Fn+Space, the
     * slider -- is the one to keep. */
    connect(hw, &HardwareControls::keyboardBacklightChanged, this, [this, hw]() {
        if (m_backlightRestore >= 0 && hw->keyboardLevel() > 0) {
            m_backlightRestore = -1;
            backlightTick();
        }
    });
    /* Not left dark when the shell quits while the light is off for idle. */
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this, hw]() {
        if (m_backlightRestore > 0)
            QProcess::startDetached(
                QStringLiteral("brightnessctl"),
                {QStringLiteral("--device"), hw->keyboardDevice(),
                 QStringLiteral("set"), QString::number(m_backlightRestore)});
    });

#ifdef FANSHELL_QS_HAVE_WAYLAND
    if (Surface::wayland()) {
        m_locked = ledLocks();
        m_ledPoll.setInterval(250);
        connect(&m_ledPoll, &QTimer::timeout, this,
                [this]() { lockStateChanged(ledLocks()); });
        m_ledPoll.start();
        /* A reload puts hyprland.lua's input settings back. */
        connect(WmState::instance(), &WmState::configReloaded, this, [this]() {
            applyOptions(true);
            applyRepeat();
            applyAtLogin();
        });
    }
#endif
    if (Display *dpy = display()) {
        XkbStateRec state;
        if (XkbGetState(dpy, XkbUseCoreKbd, &state) == Success)
            m_locked = state.locked_mods;
    }
    if (xcb_connection_t *c = connection()) {
        const xcb_query_extension_reply_t *xkb =
            xcb_get_extension_data(c, &xcb_xkb_id);
        const xcb_query_extension_reply_t *xi =
            xcb_get_extension_data(c, &xcb_input_id);
        qApp->installNativeEventFilter(new KeyboardFilter(
            xkb && xkb->present ? xkb->first_event : -1,
            xi && xi->present ? xi->major_opcode : -1));
    }

    applyOptions(false);
    applyRepeat();
    applyAtLogin();
    backlightTick();
}

void KeyboardSettings::applyAtLogin()
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* Hyprland's own option: Num Lock on whenever it sets a keymap up. */
    if (Surface::wayland()) {
        HyprIpc::setOption(QStringLiteral("input:numlock_by_default"),
                           m_settings.numlock);
        return;
    }
#endif
    if (markApplied() || !m_settings.numlock)
        return;
    quiet();
    if (!setNumlock(true))
        qWarning("fanshell-qs: no Num_Lock modifier; Num Lock left as it is");
}

void KeyboardSettings::applyRepeat()
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    if (Surface::wayland()) {
        HyprIpc::setOptions({{QStringLiteral("input:repeat_delay"), m_settings.repeatDelay},
                             {QStringLiteral("input:repeat_rate"), m_settings.repeatRate}});
        return;
    }
#endif
    Display *dpy = display();
    if (!dpy)
        return;
    XkbSetAutoRepeatRate(dpy, XkbUseCoreKbd, m_settings.repeatDelay,
                         qMax(1, 1000 / m_settings.repeatRate));
    XFlush(dpy);
}

/* setxkbmap -option, in-process: the rules, model, layout and variant come
 * from _XKB_RULES_NAMES as they are; the options the window manages are
 * replaced, and the keymap they name is compiled by the server and loaded.
 * With `force`, it is loaded even when the options already match, for a
 * keyboard that has just been added with the server's defaults. */
void KeyboardSettings::applyOptions(bool force)
{
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* input:kb_options, keeping options hyprland.lua set that this window
     * does not manage. */
    if (Surface::wayland()) {
        const QString current =
            HyprIpc::optionValue(QStringLiteral("input:kb_options")).toString();
        QStringList options;
        for (const QString &o : current.split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!managedOption(o.trimmed()))
                options << o.trimmed();
        for (const char *o : {capsChoices[choiceIndex(capsChoices, m_settings.caps)].option,
                              composeChoices[choiceIndex(composeChoices, m_settings.compose)].option})
            if (*o)
                options << QString::fromLatin1(o);
        const QString wanted = options.join(QLatin1Char(','));
        if (force || wanted != current) {
            quiet();
            HyprIpc::setOption(QStringLiteral("input:kb_options"), wanted);
        }
        return;
    }
#endif
    Display *dpy = display();
    if (!dpy)
        return;
    char *rulesName = nullptr;
    XkbRF_VarDefsRec vars;
    std::memset(&vars, 0, sizeof vars);
    if (!XkbRF_GetNamesProp(dpy, &rulesName, &vars) || !rulesName) {
        qWarning("fanshell-qs: no _XKB_RULES_NAMES; keyboard options not set");
        free(rulesName);
        return;
    }

    QStringList options;
    const QString current = QString::fromLatin1(vars.options ? vars.options : "");
    for (const QString &o : current.split(QLatin1Char(','), Qt::SkipEmptyParts))
        if (!managedOption(o))
            options << o;
    for (const char *o : {capsChoices[choiceIndex(capsChoices, m_settings.caps)].option,
                          composeChoices[choiceIndex(composeChoices, m_settings.compose)].option})
        if (*o)
            options << QString::fromLatin1(o);
    const QByteArray wanted = options.join(QLatin1Char(',')).toLatin1();

    if (force || wanted != current.toLatin1()) {
        free(vars.options);
        vars.options = wanted.isEmpty() ? nullptr : strdup(wanted.constData());
        QByteArray path = QByteArray(rulesDir) + rulesName;
        XkbRF_RulesPtr rules = XkbRF_Load(path.data(), const_cast<char *>("C"),
                                          False, True);
        XkbComponentNamesRec names;
        std::memset(&names, 0, sizeof names);
        if (rules && XkbRF_GetComponents(rules, &vars, &names)) {
            quiet();
            XkbDescPtr xkb = XkbGetKeyboardByName(
                dpy, XkbUseCoreKbd, &names, XkbGBN_AllComponentsMask,
                XkbGBN_AllComponentsMask & ~XkbGBN_GeometryMask, True);
            if (xkb) {
                XkbRF_SetNamesProp(dpy, rulesName, &vars);
                XkbFreeKeyboard(xkb, XkbAllComponentsMask, True);
            } else {
                qWarning("fanshell-qs: the server would not load the keymap "
                         "for options \"%s\"", wanted.constData());
            }
        } else {
            qWarning("fanshell-qs: cannot read the XKB rules %s", path.constData());
        }
        free(names.keycodes);
        free(names.types);
        free(names.compat);
        free(names.symbols);
        free(names.geometry);
        free(names.keymap);
        if (rules)
            XkbRF_Free(rules, True);
        XFlush(dpy);
    }
    free(rulesName);
    free(vars.model);
    free(vars.layout);
    free(vars.variant);
    free(vars.options);
}

void KeyboardSettings::set(const Settings &s)
{
    const Settings old = m_settings;
    m_settings = s;
    if (!Paths::writeConf(
            QLatin1String(keyboardConf),
            QStringLiteral("Keyboard settings, set from the Keyboard popup.\n"
                           "repeat_delay: ms; repeat_rate: per second; "
                           "backlight_idle: seconds, 0 = never."),
            {QStringLiteral("numlock=%1").arg(s.numlock ? "on" : "off"),
             QStringLiteral("repeat_delay=%1").arg(s.repeatDelay),
             QStringLiteral("repeat_rate=%1").arg(s.repeatRate),
             QStringLiteral("caps=%1").arg(s.caps),
             QStringLiteral("compose=%1").arg(s.compose),
             QStringLiteral("lock_osd=%1").arg(s.lockOsd ? "on" : "off"),
             QStringLiteral("backlight_idle=%1").arg(s.backlightIdle)}))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(keyboardConf))));
    if (s.caps != old.caps || s.compose != old.compose)
        applyOptions(false);
    if (s.repeatDelay != old.repeatDelay || s.repeatRate != old.repeatRate)
        applyRepeat();
    if (Surface::wayland()) {
        if (s.numlock != old.numlock)
            applyAtLogin();
    } else if (s.numlock && !old.numlock) {
        quiet();
        setNumlock(true);
    }
    if (s.backlightIdle != old.backlightIdle)
        backlightTick();
}

void KeyboardSettings::keyboardAdded()
{
    m_hotplug.start();
}

void KeyboardSettings::quiet()
{
    m_quietUntil = nowMs() + 500;
}

void KeyboardSettings::lockStateChanged(unsigned int lockedMods)
{
    const unsigned int changed = m_locked ^ lockedMods;
    m_locked = lockedMods;
    if (!changed || !m_settings.lockOsd || nowMs() < m_quietUntil)
        return;
    Display *dpy = display();
    const unsigned int num = dpy ? numlockMask(dpy) : Surface::wayland() ? Mod2Mask : 0;
    if (changed & LockMask)
        HardwareOsd::instance()->showLock(QStringLiteral("Caps Lock"),
                                          QStringLiteral("A"),
                                          lockedMods & LockMask);
    else if (num && (changed & num))
        HardwareOsd::instance()->showLock(QStringLiteral("Num Lock"),
                                          QStringLiteral("1"), lockedMods & num);
}

/* Sleeps until the idle limit could be reached, like the screen lock; once
 * the light is off, looks four times a second for the input that brings it
 * back. */
void KeyboardSettings::backlightTick()
{
    m_backlightTimer.stop();
    HardwareControls *hw = HardwareControls::instance();
    if (!hw->hasKeyboardBacklight() || m_settings.backlightIdle <= 0) {
#ifdef FANSHELL_QS_HAVE_WAYLAND
        delete m_backlightIdle;
        m_backlightIdle = nullptr;
#endif
        backlightRestore();
        return;
    }
#ifdef FANSHELL_QS_HAVE_WAYLAND
    /* Wayland: told when input stops and starts again. Whatever programs
     * ask of the screen, the keyboard light is about the keyboard. */
    if (Surface::wayland()) {
        delete m_backlightIdle;
        m_backlightIdle = nullptr;
        if (!Wl::IdleWatch::supported())
            return;
        m_backlightIdle = new Wl::IdleWatch(m_settings.backlightIdle * 1000, true, this);
        connect(m_backlightIdle, &Wl::IdleWatch::idled, this, [this, hw]() {
            if (hw->keyboardLevel() > 0) {
                m_backlightRestore = hw->keyboardLevel();
                hw->setKeyboardBacklight(0);
            }
        });
        connect(m_backlightIdle, &Wl::IdleWatch::resumed, this,
                &KeyboardSettings::backlightRestore);
        return;
    }
#endif
    const qint64 idle = idleMs();
    if (idle < 0)
        return;
    if (m_backlightRestore >= 0) {
        if (idle >= m_lastIdle) {
            m_lastIdle = idle;
            m_backlightTimer.start(250);
            return;
        }
        backlightRestore();
    }
    m_lastIdle = idle;
    const qint64 limit = qint64(m_settings.backlightIdle) * 1000;
    if (idle >= limit) {
        if (hw->keyboardLevel() > 0) {
            m_backlightRestore = hw->keyboardLevel();
            hw->setKeyboardBacklight(0);
            m_backlightTimer.start(250);
        } else {
            /* Already off by hand: nothing to switch off. */
            m_backlightTimer.start(5000);
        }
        return;
    }
    m_backlightTimer.start(int(std::clamp<qint64>(limit - idle + 50, 250, 60000)));
}

void KeyboardSettings::backlightRestore()
{
    if (m_backlightRestore < 0)
        return;
    const int level = m_backlightRestore;
    m_backlightRestore = -1;
    HardwareControls::instance()->setKeyboardBacklight(level);
}

void KeyboardSettings::openWindow()
{
    if (!m_win)
        m_win = new KeyboardWindow();
    m_win->open();
}

void KeyboardSettings::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void KeyboardSettings::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}
