/* Bar popups.
 *
 * A ShellPopup is a rounded, bordered top-level with Theme.popupMargin
 * padding, placed under the bar widget that owns it. Each is an
 * override-redirect window (see surface.h), and the two jobs a popup needs
 * from the display server are done like this:
 *
 *   focus    Keyboard::OnDemand: the popup takes X input focus as it shows,
 *            and hands it back to the window that had it when it hides. No
 *            keyboard grab, so the window manager's own bindings still work.
 *   dismiss  a transparent scrim window covers everything except the bar
 *            strip while a popup is open, and a click on it closes the popup.
 *            The scrim is shown first and the popup raised over it.
 *
 * Leaving the bar uncovered keeps click-to-toggle and click-another-pill
 * working. The dismissing click is consumed by the scrim rather than passed
 * to the window underneath, as in fanhypr-qs. The scrim relies on a
 * compositor (picom) to be see-through; without one it would paint black. */
#ifndef FANSHELL_QS_POPUP_H
#define FANSHELL_QS_POPUP_H

#include <QPointer>
#include <QWidget>

class QScreen;

class ShellPopup : public QWidget {
    Q_OBJECT
public:
    explicit ShellPopup(QWidget *anchor, int popupWidth);

    /* Popup top-left = the anchor's screen position plus `rel`. Default:
     * under the anchor, left-aligned, 4px gap. */
    void setAnchorOffset(const QPoint &rel);
    /* Centre the popup horizontally on its anchor instead of left-aligning
     * it. Left alignment reads fine for a pill at either end of the bar,
     * where the popup and the pill share an edge, but not for one in the
     * middle: the popup is wider than the clock, so a shared left edge just
     * looks off-centre. Ignored once setAnchorOffset() has been given an
     * explicit position. */
    void setAnchorCentered(bool centered);
    void setPopupWidth(int w);
    /* Re-point at a replacement anchor widget (e.g. a menu row rebuilt under
     * an open submenu). Takes effect on the next open/relayout. */
    void setAnchor(QWidget *anchor) { m_anchor = anchor; }

    virtual void openPopup();
    void closePopup();
    void togglePopup();

    /* Recompute size (content height changed) and reposition when visible. */
    void relayout();

signals:
    void popupVisibleChanged(bool visible);

protected:
    /* Height of the popup; default = layout()->sizeHint().height(). */
    virtual int popupHeight() const;

    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

    /* Output-local top-left this popup wants, for the given height. */
    QPoint computePos(int h) const;

    QPointer<QWidget> m_anchor;
    int m_width;
    QPoint m_rel;
    bool m_hasRel = false;
    bool m_centered = false;
    /* false for submenus: they must not close their parent via
     * PopupManager (TrayMenu topLevel). */
    bool m_manageAsTopLevel = true;

private:
    /* Surfaces are configured once, before their first map; later moves are
     * margin updates. Configuration happens in openPopup(), never in
     * relayout(): a subclass that relayouts from its constructor
     * (CalendarPopup does, to size itself) would otherwise bind the surface
     * to whatever screen the anchor reports mid-construction -- before the
     * Panel has been given one. */
    bool m_configured = false;
    /* Output the surface was configured for; a change re-configures it. */
    QPointer<QScreen> m_screen;
};

/* A popup whose height follows its content column, optionally clamped to a
 * max height (content clips, no scrolling). */
class ContentPopup : public ShellPopup {
    Q_OBJECT
public:
    ContentPopup(QWidget *anchor, int popupWidth, int maxHeight = 0);

    QWidget *body() const { return m_body; }
    class QVBoxLayout *bodyLayout() const { return m_bodyLayout; }

protected:
    int popupHeight() const override;
    void resizeEvent(QResizeEvent *) override;

private:
    QWidget *m_body;
    class QVBoxLayout *m_bodyLayout;
    int m_maxHeight;
};

namespace PopupManager {
/* Tracks the currently-open top-level popup so opening a new one closes the
 * previous, and owns the click-outside scrim. */
void opened(ShellPopup *p);
void closed(ShellPopup *p);
/* Close whatever popup is open — used when the launcher opens, so a popup
 * holding exclusive keyboard focus can't swallow its keystrokes. */
void closeCurrent();
/* Raise the scrim on `screen` ahead of mapping a popup there. */
void showScrim(QScreen *screen);
} /* namespace PopupManager */

#endif
