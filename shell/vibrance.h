/* Digital vibrance (colour saturation) per output, remembered across
 * sessions.
 *
 * The technique is libvibrant's: the kernel's per-CRTC colour transformation
 * matrix, exposed by the modesetting, amdgpu and intel X drivers as the RandR
 * output property "CTM". A saturation s becomes the 3x3 matrix with
 * (1 - s) / 3 + s on the diagonal and (1 - s) / 3 everywhere else: 1.0 is the
 * identity, 0.0 greyscale, above 1.0 more vivid. Each coefficient is S31.32
 * sign-magnitude fixed point, sent as two 32-bit INTEGER items (low word
 * first), so the property is 18 items long.
 *
 * It is done here over xcb-randr on Qt's own connection rather than by
 * linking libvibrant, which would mean libXNVCtrl and a second Xlib display.
 * Outputs without a CTM property (the NVIDIA proprietary driver) are simply
 * not offered.
 *
 * Choices are saved to vibrance.conf in Paths::configDir(), one OUTPUT=VALUE
 * line each, and applied at startup and whenever an output is (re)connected,
 * because a modeset can reset the matrix. Values for outputs that are not
 * connected right now are kept, so a docked monitor gets its setting back. */
#ifndef FANSHELL_QS_VIBRANCE_H
#define FANSHELL_QS_VIBRANCE_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

class Vibrance : public QObject {
    Q_OBJECT
public:
    /* The slider's range. libvibrant accepts up to 4.0, but past 2.0 colours
     * just clip; keeping 1.0 at the centre makes "normal" easy to find. */
    static constexpr double minimum = 0.0;
    static constexpr double maximum = 2.0;
    static constexpr double normal = 1.0;

    struct Output {
        QString name;     /* RandR name, which is also QScreen::name() */
        quint32 id = 0;   /* xcb_randr_output_t */
        double saturation = normal;
    };

    static Vibrance *instance();

    /* Connected outputs that support CTM, in RandR order. */
    const QVector<Output> &outputs() const { return m_outputs; }

    /* Applies immediately and saves shortly after (a drag saves once). */
    void setSaturation(const QString &output, double saturation);
    /* -1 when `output` has no CTM (or is not connected and on). */
    double saturation(const QString &output) const;

    /* Re-read the outputs and push saved values back; the Displays popup
     * calls this after an xrandr change, which can reset the matrix. */
    void refresh();

signals:
    /* Output set or a saturation changed. */
    void changed();

private:
    explicit Vibrance(QObject *parent = nullptr);
    void rescan();
    void restore();
    void save();

    QVector<Output> m_outputs;
    /* Everything in vibrance.conf, including outputs not connected now. */
    QHash<QString, double> m_saved;
    QTimer m_saveTimer;
    QTimer m_hotplugTimer;
};

#endif
