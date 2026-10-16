/* The top panel: one dock window per output, anchored across the top edge
 * with an exclusive zone of Theme::panelHeight. The window manager (the
 * patched oxwm, or qtile) maps docks without managing them and reserves
 * their height, so every other window tiles beneath it with nothing in its
 * config having to agree on the size. */
#ifndef FANSHELL_QS_PANEL_H
#define FANSHELL_QS_PANEL_H

#include <QPointer>
#include <QScreen>
#include <QWidget>

#include "wmstate.h"
#include "widgets.h"

class QHBoxLayout;

class WorkspaceButton : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceButton(const QString &label, QWidget *parent = nullptr);

    void setLabel(const QString &l);
    void setStates(bool selected, bool occupied, bool urgent);

signals:
    void clicked();
    void rightClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QString m_label;
    bool m_selected = false;
    bool m_occupied = false;
    bool m_urgent = false;
    bool m_hover = false;
    Qt::MouseButton m_pressed = Qt::NoButton;
};

class Panel : public QWidget {
    Q_OBJECT
public:
    Panel(WmState *state, QScreen *screen);

    /* Max characters shown for the window title before shortening. */
    static constexpr int maxTitleLength = 45;
    static QString shortenTitle(const QString &t);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    const WmState::Monitor *mon() const;
    bool hostsTray() const;
    void syncFromState();
    void syncTitle();
    /* Positions m_leftBox / m_clock / m_rightBox: the clock sits at a fixed
     * horizontal center of the bar (clamped so it never collides with the
     * right-side pills), and m_leftBox gets whatever space is left of it. */
    void relayoutBar();

    WmState *m_state;
    QPointer<QScreen> m_screen;

    QWidget *m_leftBox;
    QWidget *m_wsBox;
    QHBoxLayout *m_wsLayout;
    QVector<WorkspaceButton *> m_wsButtons;
    /* Tag names currently backing m_wsButtons, so the row is only rebuilt
     * when the config's tag set actually changes. */
    QStringList m_wsTags;
    TextItem *m_title;

    QWidget *m_clock;
    QWidget *m_rightBox;
};

#endif
