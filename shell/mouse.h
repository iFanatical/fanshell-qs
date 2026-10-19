/* Mouse and touchpad settings, from the Control Panel's Mouse & touchpad
 * tile:
 *
 *     fanshell-qs-shell ipc call mouse toggle|show|hide
 *
 * Per device, for devices on the libinput X driver: pointer speed, the
 * acceleration profile (adaptive or flat), natural scrolling and
 * left-handed buttons; on a touchpad also tap to click, disable while
 * typing, and two-finger or edge scrolling. They are the driver's XInput
 * device properties ("libinput Accel Speed", ...), set over xcb as
 * `xinput set-prop` would, so xinput is not needed.
 *
 * Settings follow a device by its name, so they come back when it is
 * plugged in again (they are applied on the XInput hierarchy event):
 *
 *   ~/.config/fanshell-qs/mouse.conf           what this popup set, by the
 *                                              device's exact name
 *   ~/.config/fanshell-qs/mouse.defaults.conf  linked from the repo: the
 *                                              same lines with any part of
 *                                              a name, matched ignoring
 *                                              case, for every machine
 *
 * Both are KEY=VALUE lines with the key SETTING:NAME, e.g.
 * `speed:Logitech G Pro=-0.50`. A setting in neither keeps the driver's
 * default. */
#ifndef FANSHELL_QS_MOUSE_H
#define FANSHELL_QS_MOUSE_H

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

class MouseWindow;

class MouseSettings : public QObject {
public:
    /* One pointer device by name; a mouse can show up as several XInput
     * devices of the same name, and each gets the same settings. */
    struct Device {
        QString name;
        QVector<int> ids;
        bool touchpad = false;
    };

    static MouseSettings *instance();

    /* The libinput pointer devices plugged in now. */
    QVector<Device> devices() const;

    /* What the device is set to now, as stored: "-0.50", "flat", "on",
     * "two-finger", ...; empty when it lacks the setting. */
    QString current(const Device &d, const QString &key) const;
    /* The choices the device offers for "accel" and "scroll". */
    QStringList available(const Device &d, const QString &key) const;
    bool has(const Device &d, const QString &key) const;

    /* Apply to the device now and save for its name. */
    void set(const Device &d, const QString &key, const QString &value);
    /* Back to the driver's defaults, then to mouse.defaults.conf's. */
    void reset(const Device &d);

    void openWindow();
    void toggleWindow();
    void hideWindow();

    /* From the X event filter. */
    void pointerAdded();

private:
    explicit MouseSettings(QObject *parent = nullptr);
    void load();
    void save();
    void applyAll();
    void apply(const Device &d);
    QString wanted(const QString &name, const QString &key) const;

    /* mouse.conf: name -> key -> value. */
    QHash<QString, QHash<QString, QString>> m_saved;
    /* mouse.defaults.conf: (lowercase part of a name, key, value). */
    struct Rule {
        QString pattern;
        QString key;
        QString value;
    };
    QVector<Rule> m_defaults;

    MouseWindow *m_win = nullptr;
    QTimer m_hotplug;   /* collapses a burst of hierarchy events */
    QTimer m_saveTimer; /* the speed slider saves once it rests */
};

#endif
