/* The centred popup the shell's settings windows share (Displays, Screen
 * power, Screen lock, the Control Panel): a box on the focused output, over
 * a transparent full-screen window that closes on a click outside the box,
 * plus the drop-down list their selectors open. Header-only; neither class
 * has signals. */
#ifndef FANSHELL_QS_OVERLAY_H
#define FANSHELL_QS_OVERLAY_H

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QWheelEvent>
#include <QWidget>

#include <functional>

#include "popup.h"
#include "surface.h"
#include "theme.h"
#include "wmstate.h"

/* The open part of a Selector: a list drawn over the whole popup window, so
 * a click anywhere outside it closes it without reaching what is beneath. */
class ChoiceList : public QWidget {
public:
    explicit ChoiceList(QWidget *window) : QWidget(window)
    {
        setMouseTracking(true);
        hide();
    }

    void open(QWidget *anchor, const QStringList &items, int current,
              std::function<void(int)> pick)
    {
        if (items.isEmpty())
            return;
        m_items = items;
        m_current = current;
        m_hover = -1;
        m_pick = std::move(pick);
        setGeometry(parentWidget()->rect());
        const int rows = qMin(int(items.size()), maxRows);
        const QPoint below =
            anchor->mapTo(parentWidget(), QPoint(0, anchor->height() + 2));
        m_box = QRect(below.x(), below.y(), anchor->width(),
                      rows * rowHeight + 2 * pad);
        if (m_box.bottom() > height() - 8)
            m_box.moveBottom(anchor->mapTo(parentWidget(), QPoint(0, -2)).y());
        m_scroll = qBound(0, current - rows / 2, int(items.size()) - rows);
        show();
        raise();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, m_box, Theme::bg, Theme::radius, Theme::border, 1);
        p.setFont(Theme::font(Theme::smallFontSize));
        for (int r = 0; r < visibleRows(); r++) {
            const int i = m_scroll + r;
            const QRect row = rowRect(r);
            if (i == m_hover || i == m_current)
                Theme::paintRect(p, row,
                                 i == m_hover ? Theme::surfaceHover
                                              : Theme::surface,
                                 Theme::smallRadius);
            p.setPen(i == m_current ? Theme::accent : Theme::text);
            p.drawText(row.adjusted(8, 0, -8, 0),
                       Qt::AlignLeft | Qt::AlignVCenter, m_items[i]);
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int i = indexAt(e->position().toPoint());
        if (i != m_hover) {
            m_hover = i;
            update();
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const int i = indexAt(e->position().toPoint());
        hide();
        if (i >= 0 && m_pick) {
            const auto pick = m_pick;
            pick(i);
        }
    }

    void wheelEvent(QWheelEvent *e) override
    {
        const int steps = e->angleDelta().y() / 120;
        m_scroll = qBound(0, m_scroll - steps,
                          int(m_items.size()) - visibleRows());
        m_hover = indexAt(e->position().toPoint());
        update();
    }

private:
    static constexpr int maxRows = 10;
    static constexpr int rowHeight = 24;
    static constexpr int pad = 4;

    int visibleRows() const { return qMin(int(m_items.size()), maxRows); }
    QRect rowRect(int r) const
    {
        return QRect(m_box.x() + pad, m_box.y() + pad + r * rowHeight,
                     m_box.width() - 2 * pad, rowHeight);
    }
    int indexAt(const QPoint &pos) const
    {
        for (int r = 0; r < visibleRows(); r++)
            if (rowRect(r).contains(pos))
                return m_scroll + r;
        return -1;
    }

    QStringList m_items;
    int m_current = -1;
    int m_hover = -1;
    int m_scroll = 0;
    QRect m_box;
    std::function<void(int)> m_pick;
};

/* A box centred on the focused output, over a transparent full-screen
 * window that dismisses on a click outside the box -- the wallpaper picker's
 * mould. Subclasses fill box() and override dismiss() to tidy up first. */
class CenteredOverlay : public QWidget {
public:
    CenteredOverlay(const QString &title, const QString &scope, int boxWidth)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint),
          m_scope(scope), m_boxWidth(boxWidth)
    {
        setWindowTitle(title);
        setAttribute(Qt::WA_TranslucentBackground);
        m_box = new QWidget(this);
        m_list = new ChoiceList(this);
    }

    virtual void dismiss()
    {
        m_list->hide();
        hide();
    }

protected:
    QWidget *box() const { return m_box; }
    ChoiceList *choiceList() const { return m_list; }

    void present()
    {
        PopupManager::closeCurrent();
        QScreen *screen = QGuiApplication::primaryScreen();
        const QString focused = WmState::instance()->focusedMonitor();
        for (QScreen *sc : QGuiApplication::screens())
            if (sc->name() == focused)
                screen = sc;
        if (!screen)
            return;

        if (m_configured && m_screen != screen) {
            hide();
            m_configured = false;
        }
        resize(screen->geometry().size());
        if (!m_configured) {
            Surface::SurfaceSpec spec;
            spec.layer = Surface::Layer::Overlay;
            spec.anchors = Surface::AnchorTop | Surface::AnchorBottom
                           | Surface::AnchorLeft | Surface::AnchorRight;
            spec.desiredSize = QSize(0, 0);
            spec.exclusiveZone = -1;
            spec.keyboard = Surface::Keyboard::Exclusive;
            spec.type = Surface::WindowType::Dialog;
            spec.scope = m_scope;
            Surface::configure(this, screen, spec);
            m_screen = screen;
            m_configured = true;
        }
        show();
        layoutBox();
        setFocus(Qt::OtherFocusReason);
    }

    /* The screen this window covers may have moved, resized or gone after
     * an xrandr change; place it afresh. */
    void reanchor()
    {
        if (!isVisible())
            return;
        hide();
        m_configured = false;
        present();
    }

    void layoutBox()
    {
        const int h = m_box->layout()->sizeHint().height();
        m_box->setGeometry((width() - m_boxWidth) / 2, (height() - h) / 2,
                           m_boxWidth, h);
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, m_box->geometry(), Theme::bg, Theme::radius,
                         Theme::border, 1);
    }
    void resizeEvent(QResizeEvent *) override { layoutBox(); }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (!m_box->geometry().contains(e->position().toPoint()))
            dismiss();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (m_list->isVisible())
                m_list->hide();
            else
                dismiss();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    QWidget *m_box;
    ChoiceList *m_list;
    QString m_scope;
    int m_boxWidth;
    bool m_configured = false;
    QPointer<QScreen> m_screen;
};

#endif
