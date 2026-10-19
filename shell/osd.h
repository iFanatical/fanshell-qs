/* Short-lived bottom-centre hardware OSD. It is an independent layer
 * surface: no panel widget or existing popup is resized or replaced. */
#ifndef FANSHELL_QS_OSD_H
#define FANSHELL_QS_OSD_H

#include <QColor>
#include <QVariantMap>
#include <QWidget>

class QPainter;
class QRectF;
class QScreen;
class QTimer;

class HardwareOsd : public QWidget {
    Q_OBJECT
public:
    static HardwareOsd *instance();
    /* Nothing is shown while set: the screen is locked, and the OSD would
     * either cover the lock screen or fight it for the top. */
    static void setSuppressed(bool suppressed);
    bool showNotification(uint id, const QString &appName,
                          const QString &summary, const QString &body,
                          const QVariantMap &hints, int urgency);
    void showVolume(int percent, bool muted);
    void showMicrophone(int percent, bool muted);
    void showBrightness(int percent, bool external);
    void showKeyboardBacklight(int percent);
    /* Caps Lock or Num Lock switched; `glyph` is drawn on the key ("A",
     * "1"). */
    void showLock(const QString &name, const QString &glyph, bool on);
    void showVpn(bool connected, const QString &interfaceName,
                 const QString &ipAddress);
    void showMedia(const QString &action, const QString &title,
                   const QString &artist);
    void showMediaUnavailable();

signals:
    void closed(uint id, uint reason);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    enum class Kind { Volume, Microphone, Brightness, Keyboard, Lock, Vpn, Media };

    explicit HardwareOsd(QWidget *parent = nullptr);
    void present();

    static bool s_suppressed;
    void configureFor(QScreen *screen);
    void drawIcon(QPainter &p, const QRectF &rect) const;

    QTimer *m_timer;
    QScreen *m_screen = nullptr;
    uint m_id = 0;
    Kind m_kind = Kind::Volume;
    int m_value = 0;
    bool m_muted = false;
    bool m_showProgress = true;
    QString m_label;
    QString m_state;
    QString m_mediaAction;
    QString m_lockGlyph;
    QColor m_accent;
    bool m_configured = false;
};

#endif
