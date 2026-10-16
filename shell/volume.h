/* Audio backend for the System dropdown's Output/Input section
 * (system.cpp).
 *
 * Volume is read and written through libpulse natively: the
 * event-driven backend is libpulse against pipewire-pulse (volumes read and
 * written on the same 0..1 scale wpctl displays). */
#ifndef FANSHELL_QS_VOLUME_H
#define FANSHELL_QS_VOLUME_H

#include <QVector>

#include "widgets.h"

struct pa_threaded_mainloop;
struct pa_context;

class PulseBackend : public QObject {
    Q_OBJECT
public:
    struct Sink {
        QString name;
        QString description;
        bool isDefault = false;
    };

    static PulseBackend *instance();

    /* Default sink (output) / source (mic) state. */
    qreal sinkVolume = 0;   /* 0..1 */
    bool sinkMuted = false;
    bool hasSink = false;
    qreal sourceVolume = 0;
    bool sourceMuted = false;
    bool hasSource = false;
    QVector<Sink> sinks;

    void setSinkVolume(qreal v);
    void setSinkMuted(bool m);
    void toggleSinkMuted() { setSinkMuted(!sinkMuted); }
    void setSourceVolume(qreal v);
    void setSourceMuted(bool m);
    void toggleSourceMuted() { setSourceMuted(!sourceMuted); }
    void setDefaultSink(const QString &name);

signals:
    void changed();

private:
    explicit PulseBackend(QObject *parent = nullptr);
    void start();
    void newContext();
    /* GUI thread: the server dropped us; reset state and reconnect. */
    void connectionLost();

    friend struct PulseCallbacks;
    struct Priv;
    Priv *d;
};

/* A QML Text with a MouseArea: fixed-width clickable glyph. Used by
 * system.cpp's audio section (the standalone volume bar pill/popup were
 * folded into the System dropdown). */
class ClickableText : public TextItem {
    Q_OBJECT
public:
    explicit ClickableText(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    bool m_pressed = false;
};

#endif
