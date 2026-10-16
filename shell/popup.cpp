#include "popup.h"

#include <QHash>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QVBoxLayout>

#include <utility>

#include "theme.h"
#include "surface.h"

/* Visible popups, most recent last. */
static QList<QPointer<ShellPopup>> g_stack;
static ShellPopup *g_current = nullptr;

namespace {

/* Transparent full-screen catcher that closes the open popup when clicked.
 * Anchored to all four edges but inset by the bar height, so the bar strip
 * itself stays clickable and click-another-pill keeps working. Shown before
 * the popup, which is then raised over it. */
class PopupScrim : public QWidget {
public:
    explicit PopupScrim(QScreen *screen)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        if (screen) {
            const QRect g = screen->geometry();
            resize(g.width(), qMax(1, g.height() - Theme::panelHeight));
        }
        Surface::SurfaceSpec spec;
        spec.layer = Surface::Layer::Top;
        spec.anchors = Surface::AnchorTop | Surface::AnchorBottom
                       | Surface::AnchorLeft | Surface::AnchorRight;
        spec.margins = QMargins(0, Theme::panelHeight, 0, 0);
        spec.desiredSize = QSize(0, 0); /* both axes: fill the output */
        spec.exclusiveZone = -1;
        spec.keyboard = Surface::Keyboard::None;
        spec.type = Surface::WindowType::Utility;
        spec.scope = QStringLiteral("fanshell-qs-scrim");
        Surface::configure(this, screen, spec);
    }

protected:
    /* Paint an explicitly near-transparent fill rather than nothing, so the
     * window is guaranteed real contents for the compositor to blend. */
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), QColor(0, 0, 0, 1));
    }

    void mousePressEvent(QMouseEvent *) override
    {
        PopupManager::closeCurrent();
    }
};

QHash<QScreen *, PopupScrim *> g_scrims;

PopupScrim *scrimFor(QScreen *screen)
{
    if (!screen)
        return nullptr;
    auto it = g_scrims.constFind(screen);
    if (it != g_scrims.constEnd())
        return *it;
    auto *s = new PopupScrim(screen);
    g_scrims.insert(screen, s);
    QObject::connect(screen, &QObject::destroyed, s, [screen]() {
        if (PopupScrim *dead = g_scrims.take(screen))
            dead->deleteLater();
    });
    return s;
}

void hideScrims()
{
    for (PopupScrim *s : std::as_const(g_scrims))
        s->hide();
}

} /* namespace */

namespace PopupManager {

void showScrim(QScreen *screen)
{
    if (PopupScrim *s = scrimFor(screen))
        s->show();
}

void opened(ShellPopup *p)
{
    /* Opening any new top-level popup tears down whatever tree (the previous
     * top-level plus any submenus stacked on it, e.g. Bluetooth's popup
     * floating over the System dropdown) was open before -- otherwise a
     * submenu that isn't itself top-level would be orphaned with nothing
     * behind it. */
    if (g_current != p) {
        if (g_current && g_current->isVisible())
            g_current->closePopup();
        for (int i = g_stack.size() - 1; i >= 0; i--)
            if (g_stack[i] && g_stack[i] != p && g_stack[i]->isVisible())
                g_stack[i]->closePopup();
    }
    g_current = p;
}

void closed(ShellPopup *p)
{
    if (g_current == p)
        g_current = nullptr;
}

void closeCurrent()
{
    if (g_current && g_current->isVisible())
        g_current->closePopup();
    /* Belt and braces: a submenu (not tracked as top-level) could still be up
     * if its parent chain got confused. */
    for (int i = g_stack.size() - 1; i >= 0; i--)
        if (g_stack[i] && g_stack[i]->isVisible())
            g_stack[i]->closePopup();
    hideScrims();
}

} /* namespace PopupManager */

/* --------------------------------------------------------------- ShellPopup */

ShellPopup::ShellPopup(QWidget *anchor, int popupWidth)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint),
      m_anchor(anchor), m_width(popupWidth)
{
    setAttribute(Qt::WA_TranslucentBackground);
}

void ShellPopup::setAnchorOffset(const QPoint &rel)
{
    m_rel = rel;
    m_hasRel = true;
}

void ShellPopup::setAnchorCentered(bool centered)
{
    m_centered = centered;
}

void ShellPopup::setPopupWidth(int w)
{
    m_width = w;
}

int ShellPopup::popupHeight() const
{
    return layout() ? layout()->sizeHint().height() : height();
}

QPoint ShellPopup::computePos(int h) const
{
    if (!m_anchor)
        return QPoint();
    QPoint rel = m_rel;
    if (!m_hasRel) {
        /* Width is only known now: a ContentPopup sizes itself from its
         * content, and setPopupWidth() can change it between opens. */
        const int dx = m_centered ? (m_anchor->width() - m_width) / 2 : 0;
        rel = QPoint(dx, m_anchor->height() + Theme::listSpacing);
    }
    QPoint pos = Surface::screenPos(m_anchor, rel);

    /* Slide to stay on the anchor's output. Margins are relative to the
     * top-left of that output (see surface.h), so this works in output-local
     * coordinates. */
    if (QScreen *scr = m_anchor->screen()) {
        const QSize sg = scr->geometry().size();
        pos.setX(qBound(0, pos.x(), qMax(0, sg.width() - m_width)));
        pos.setY(qBound(0, pos.y(), qMax(0, sg.height() - h)));
    }
    return pos;
}

void ShellPopup::relayout()
{
    const int h = popupHeight();
    setFixedSize(m_width, h);
    /* Nothing to move before the surface exists -- openPopup() places it. */
    if (!m_configured || !m_anchor)
        return;
    const QPoint pos = computePos(h);
    Surface::setMargins(this, QMargins(pos.x(), pos.y(), 0, 0));
    Surface::setDesiredSize(this, QSize(m_width, h));
}

void ShellPopup::openPopup()
{
    const int h = popupHeight();
    setFixedSize(m_width, h);

    QScreen *scr = m_anchor ? m_anchor->screen() : nullptr;
    if (m_configured && m_screen != scr) {
        /* The anchor moved to another output: re-configure against it. */
        m_configured = false;
    }

    const QPoint pos = computePos(h);
    const QMargins margins(pos.x(), pos.y(), 0, 0);
    if (!m_configured) {
        Surface::SurfaceSpec spec;
        spec.layer = Surface::Layer::Overlay;
        spec.anchors = Surface::AnchorTop | Surface::AnchorLeft;
        spec.margins = margins;
        spec.desiredSize = QSize(m_width, h); /* anchored one corner: ours */
        spec.exclusiveZone = -1; /* measure margins from the true screen edge */
        /* OnDemand + activateOnShow: focused as it maps, without a keyboard
         * grab, so the scrim and the bar still get their clicks and the WM
         * its key bindings. */
        spec.keyboard = Surface::Keyboard::OnDemand;
        spec.activateOnShow = true;
        Surface::configure(this, scr, spec);
        m_screen = scr;
        m_configured = true;
    } else {
        Surface::setMargins(this, margins);
        Surface::setDesiredSize(this, QSize(m_width, h));
    }

    /* Scrim first: it must already be mapped when the popup appears, so a
     * click anywhere outside has something to land on. */
    if (m_manageAsTopLevel && m_anchor)
        PopupManager::showScrim(scr);
    show();
}

void ShellPopup::closePopup()
{
    hide();
}

void ShellPopup::togglePopup()
{
    if (isVisible())
        closePopup();
    else
        openPopup();
}

void ShellPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    Theme::paintRect(p, rect(), Theme::bg, Theme::radius, Theme::border, 1);
}

void ShellPopup::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        closePopup();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ShellPopup::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    g_stack.removeAll(this);
    g_stack.append(this);
    if (m_manageAsTopLevel)
        PopupManager::opened(this);
    emit popupVisibleChanged(true);
}

void ShellPopup::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (m_manageAsTopLevel)
        PopupManager::closed(this);
    g_stack.removeAll(this);
    /* Keyboard focus is unwound by Surface: hiding hands it back to whatever
     * had it before, including a popup still up beneath this one. */
    bool anyVisible = false;
    for (const QPointer<ShellPopup> &p : std::as_const(g_stack))
        if (p && p->isVisible())
            anyVisible = true;
    if (!anyVisible)
        hideScrims();
    emit popupVisibleChanged(false);
}

/* ------------------------------------------------------------- ContentPopup */

ContentPopup::ContentPopup(QWidget *anchor, int popupWidth, int maxHeight)
    : ShellPopup(anchor, popupWidth), m_maxHeight(maxHeight)
{
    m_body = new QWidget(this);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                     Theme::popupMargin, Theme::popupMargin);
    m_bodyLayout->setSpacing(Theme::popupSpacing);
    m_body->show();
}

int ContentPopup::popupHeight() const
{
    const int h = m_body->sizeHint().height();
    return m_maxHeight > 0 ? qMin(m_maxHeight, h) : h;
}

void ContentPopup::resizeEvent(QResizeEvent *e)
{
    ShellPopup::resizeEvent(e);
    /* Full content height; anything past the popup's clamped height clips. */
    m_body->setGeometry(0, 0, width(), m_body->sizeHint().height());
}
