/* Common leaf widgets: BarPill, ShellButton,
 * ValueSlider, plus TextItem (a QML-Text-alike label) and HLine. */
#ifndef FANSHELL_QS_WIDGETS_H
#define FANSHELL_QS_WIDGETS_H

#include <QWidget>
#include <QVector>

#include "theme.h"

/* QML Text equivalent: tight font-metrics sizing, optional right-elide,
 * vertical centering. */
class TextItem : public QWidget {
    Q_OBJECT
public:
    explicit TextItem(QWidget *parent = nullptr);

    void setText(const QString &t, bool updateLayout = true);
    QString text() const { return m_text; }
    void setColor(const QColor &c);
    void setPixelSize(int px);
    void setBold(bool b);
    void setElide(bool e);            /* elide right when wider than width() */
    void setHAlign(Qt::Alignment a);  /* AlignLeft (default) / Center / Right */

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_text;
    QColor m_color = Theme::text;
    int m_pixelSize = Theme::panelFontSize;
    bool m_bold = false;
    bool m_elide = false;
    Qt::Alignment m_halign = Qt::AlignLeft;
};

/* Clickable bar pill with optional glyph + label. The label can
 * be a list of independently coloured segments (QML richLabel/StyledText). */
class BarPill : public QWidget {
    Q_OBJECT
public:
    struct Segment { QString text; QColor color; };

    explicit BarPill(QWidget *parent = nullptr);

    void setIcon(const QString &icon);
    void setLabel(const QString &label);
    void setRichSegments(const QVector<Segment> &segs); /* replaces label */
    void setTint(const QColor &c);
    void setActive(bool a);
    bool isActive() const { return m_active; }
    bool highlighted() const { return m_hover || m_active; }
    /* ~2x font/height/padding -- for pills embedded as bigger tiles inside a
     * popup (e.g. system.cpp's Bluetooth/VPN/Clipboard row) rather than the
     * normal bar row. */
    void setLarge(bool l);
    /* Bold icon and label, for a pill whose short figure should stand out
     * (the battery's percentage). */
    void setBold(bool b);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void clicked();
    void rightClicked();
    void scrolled(int delta);   /* +1 = up/away, -1 = down/toward */
    void hoverChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    qreal contentWidth() const;

    QString m_icon;
    QString m_label;
    QVector<Segment> m_rich;
    bool m_useRich = false;
    QColor m_tint = Theme::text;
    bool m_active = false;
    bool m_hover = false;
    bool m_large = false;
    bool m_bold = false;
    Qt::MouseButton m_pressedButton = Qt::NoButton;
};

/* A themed push button. */
class ShellButton : public QWidget {
    Q_OBJECT
public:
    explicit ShellButton(const QString &label = QString(), QWidget *parent = nullptr);

    void setLabel(const QString &l);
    void setDanger(bool d);
    void setActive(bool a);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void activated();

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override;

private:
    QString m_label;
    bool m_danger = false;
    bool m_active = false;
    bool m_hover = false;
    bool m_pressed = false;
};

/* Minimal 0..1 horizontal slider. */
class ValueSlider : public QWidget {
    Q_OBJECT
public:
    explicit ValueSlider(QWidget *parent = nullptr);

    void setValue(qreal v);
    void setFillColor(const QColor &c);

    QSize sizeHint() const override { return QSize(60, 16); }

signals:
    void moved(qreal v);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    qreal m_value = 0;
    QColor m_fill = Theme::accent;
    bool m_dragging = false;
};

/* 1px full-width separator (Rectangle { height: 1; color: Theme.border }) */
class HLine : public QWidget {
    Q_OBJECT
public:
    explicit HLine(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(1, 1); }
protected:
    void paintEvent(QPaintEvent *) override;
};

#endif
