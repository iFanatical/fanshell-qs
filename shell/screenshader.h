/* Colour on Hyprland: digital vibrance and the night light, in one screen
 * shader (the Wayland counterpart of the RandR CTM and gamma ramps that
 * vibrance.cpp and nightlight.cpp write on X11).
 *
 * Hyprland's colour-matrix protocol (hyprland-ctm-control-v1) only takes
 * matrix entries >= 0, which can warm the screen but never boost
 * saturation, and only one client may hold it. So, as hyprvibrance does,
 * the shell generates a fragment shader and sets it as
 * decoration:screen_shader. Hyprland gives every screen shader a
 * `wl_output` uniform holding the id of the monitor being drawn, so one
 * shader carries a saturation per output; the night light is a gain per
 * channel on top, the same on every output.
 *
 * The shader is $XDG_RUNTIME_DIR/fanshell-qs/screen.frag, replaced
 * atomically. With nothing to do (every output at 100%, the night light
 * neutral) the option is handed back -- to a shader someone else had set,
 * or to none -- so no shader runs over every frame for nothing. A config
 * reload resets the option, and hotplug renumbers the monitors, so both
 * write it again. hyprvibrance and hyprsunset are not needed alongside, and
 * would fight over the same slots. */
#ifndef FANSHELL_QS_SCREENSHADER_H
#define FANSHELL_QS_SCREENSHADER_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class ScreenShader : public QObject {
    Q_OBJECT
public:
    static ScreenShader *instance();

    /* 1.0 is normal; per output, by name. */
    void setSaturation(const QString &output, double saturation);
    /* The night light's per-channel gains, 1.0 each for neutral. */
    void setGain(double r, double g, double b);

private:
    explicit ScreenShader(QObject *parent = nullptr);
    void schedule();
    void write();
    QString path() const;

    QHash<QString, double> m_saturation;
    double m_gain[3] = {1.0, 1.0, 1.0};
    QTimer m_write;
    /* decoration:screen_shader as it was before the shell took it. */
    QString m_foreign;
    bool m_ours = false;
};

#endif
