#include "keyboard.h"

#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "overlay.h"
#include "paths.h"
#include "widgets.h"

/* Last: Xlib's macros (None, Bool, Status, ...) collide with Qt's names. */
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

namespace {

constexpr auto keyboardConf = "keyboard.conf";
/* On the root window once Num Lock has been set for this X session. */
constexpr auto appliedAtom = "_FANSHELL_QS_NUMLOCK_APPLIED";

Display *display()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->display() : nullptr;
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

} /* namespace */

class KeyboardWindow : public CenteredOverlay {
public:
    KeyboardWindow()
        : CenteredOverlay(QStringLiteral("fanshell-keyboard"),
                          QStringLiteral("fanshell-qs-keyboard"), 400)
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
        auto *label = new TextItem(box());
        label->setPixelSize(Theme::smallFontSize);
        label->setColor(Theme::textMuted);
        label->setText(QStringLiteral("Num Lock at login"));
        m_numlock = new ShellButton(QStringLiteral("Off"), box());
        auto *row = new QWidget(box());
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->addWidget(m_numlock);
        rl->addStretch(1);
        grid->addWidget(label, 0, 0);
        grid->addWidget(row, 0, 1);
        lay->addLayout(grid);

        auto *note = new TextItem(box());
        note->setPixelSize(Theme::tinyFontSize);
        note->setColor(Theme::textMuted);
        note->setText(QStringLiteral("Off leaves Num Lock as the keyboard starts."));
        lay->addWidget(note);

        lay->addWidget(new HLine(box()));
        auto *footer = new QWidget(box());
        auto *fl = new QHBoxLayout(footer);
        fl->setContentsMargins(0, 0, 0, 0);
        auto *close = new ShellButton(QStringLiteral("Close"), footer);
        fl->addStretch(1);
        fl->addWidget(close);
        lay->addWidget(footer);

        connect(m_numlock, &ShellButton::activated, this, [this]() {
            KeyboardSettings::Settings s = KeyboardSettings::instance()->settings();
            s.numlock = !s.numlock;
            KeyboardSettings::instance()->set(s);
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
        const bool on = KeyboardSettings::instance()->settings().numlock;
        m_numlock->setLabel(on ? QStringLiteral("On") : QStringLiteral("Off"));
        m_numlock->setActive(on);
    }

    ShellButton *m_numlock;
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
    if (conf.contains(QStringLiteral("numlock")))
        m_settings.numlock = conf.value(QStringLiteral("numlock")) != QLatin1String("off");
    applyAtLogin();
}

void KeyboardSettings::applyAtLogin()
{
    if (markApplied() || !m_settings.numlock)
        return;
    if (!setNumlock(true))
        qWarning("fanshell-qs: no Num_Lock modifier; Num Lock left as it is");
}

void KeyboardSettings::set(const Settings &s)
{
    m_settings = s;
    if (!Paths::writeConf(
            QLatin1String(keyboardConf),
            QStringLiteral("Keyboard settings, set from the Keyboard popup.\n"
                           "numlock: Num Lock on at login (on/off)."),
            {QStringLiteral("numlock=%1").arg(s.numlock ? "on" : "off")}))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(keyboardConf))));
    if (s.numlock)
        setNumlock(true);
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
