/* Colour picker and screenshots, over a frozen copy of the screen.
 *
 *     fanshell-qs-shell ipc call picker pick
 *     fanshell-qs-shell ipc call screenshot region|output|screen
 *
 * Both start by copying the screen as picom shows it (translucency and
 * all), so what is picked or captured is what was on screen at the key
 * press, even if it changes meanwhile.
 *
 *   picker   a loupe follows the pointer over the frozen screen; a click
 *            copies the pixel's colour as #rrggbb to the clipboard and says
 *            so in a notification. Escape or right-click cancels.
 *   region   drag a rectangle (across outputs if need be); a click without
 *            dragging takes the whole output under the pointer. Escape or
 *            right-click cancels.
 *   output   the output under the pointer, at once.
 *   screen   every output, at once.
 *
 * Under Hyprland the copy comes from grim (screencopy), and the overlay is
 * a layer surface per output.
 *
 * A screenshot is saved as a PNG in screenshot.dir (shell.conf; default
 * ~/Pictures/screenshots), copied to the clipboard, and announced with a
 * thumbnail. */
#ifndef FANSHELL_QS_CAPTURE_H
#define FANSHELL_QS_CAPTURE_H

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QVector>

class CaptureWindow;

class Capture : public QObject {
    Q_OBJECT
public:
    enum class Kind { Pick, Region, Output, Screen };

    static Capture *instance();

    void pickColor() { begin(Kind::Pick); }
    void screenshot(Kind kind) { begin(kind); }
    void begin(Kind kind);

private:
    friend class CaptureWindow;
    explicit Capture(QObject *parent = nullptr);

    /* From the overlay windows, in global coordinates. */
    void pressed(const QPoint &global);
    void moved(const QPoint &global);
    void released(const QPoint &global);
    void cancel();

    QRect selection() const;
    void finishPick(const QPoint &global);
    void finishShot(const QRect &global);
    void close();

    Kind m_kind = Kind::Pick;
    QImage m_frozen;
    /* Where m_frozen's top-left pixel is on the desktop (Hyprland can put
     * outputs left of or above 0,0). */
    QPoint m_frozenOrigin;
    QVector<CaptureWindow *> m_windows;
    QPoint m_pointer;
    QPoint m_anchor;
    bool m_dragging = false;
};

#endif
