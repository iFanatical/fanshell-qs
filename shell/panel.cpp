#include "panel.h"

#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>

#include "battery.h"
#include "calendar.h"
#include "network.h"
#include "notification.h"
#include "pills.h"
#include "status.h"
#include "system.h"
#include "tray.h"
#include "surface.h"

/* --------------------------------------------------------- WorkspaceButton */

WorkspaceButton::WorkspaceButton(const QString &label, QWidget *parent)
    : QWidget(parent), m_label(label)
{
    setFixedSize(Theme::workspaceButtonSize, Theme::workspaceButtonSize);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void WorkspaceButton::setLabel(const QString &l)
{
    if (m_label == l)
        return;
    m_label = l;
    update();
}

void WorkspaceButton::setStates(bool selected, bool occupied, bool urgent)
{
    if (m_selected == selected && m_occupied == occupied && m_urgent == urgent)
        return;
    m_selected = selected;
    m_occupied = occupied;
    m_urgent = urgent;
    update();
}

void WorkspaceButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QColor fill = m_selected
                            ? Theme::surface
                            : (m_hover ? Theme::surfaceHover
                                       : Theme::transparent);
    Theme::paintRect(p, rect(), fill, Theme::smallRadius);
    /* urgent = red, active = purple/pink, has windows = white, empty = gray */
    p.setFont(Theme::font(Theme::panelFontSize,
                          m_selected || m_occupied || m_urgent));
    p.setPen(m_urgent ? Theme::urgent
             : m_selected ? Theme::tagActive
                          : (m_occupied ? Theme::tagOccupied : Theme::tagEmpty));
    p.drawText(rect(), Qt::AlignCenter, m_label);
}

void WorkspaceButton::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}

void WorkspaceButton::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void WorkspaceButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)
        m_pressed = e->button();
}

/* dwm's convention: left views the tag alone, right toggles it into view. */
void WorkspaceButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_pressed == e->button() && rect().contains(e->pos())) {
        if (m_pressed == Qt::LeftButton)
            emit clicked();
        else if (m_pressed == Qt::RightButton)
            emit rightClicked();
    }
    m_pressed = Qt::NoButton;
}

/* ------------------------------------------------------------------- Panel */

Panel::Panel(WmState *state, QScreen *screen)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint),
      m_state(state), m_screen(screen)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);

    /* Left group (workspaces / layout symbol / title) and the right-side pill
     * cluster each get their own container so the clock can be positioned at
     * a fixed bar-center in relayoutBar(), independent of how wide either
     * side happens to be — see panel.h. */
    m_leftBox = new QWidget(this);
    auto *leftRow = new QHBoxLayout(m_leftBox);
    leftRow->setContentsMargins(0, 0, 0, 0);
    leftRow->setSpacing(Theme::rowSpacing);

    /* Workspaces, as this output currently shows them. */
    m_wsBox = new QWidget(m_leftBox);
    m_wsLayout = new QHBoxLayout(m_wsBox);
    m_wsLayout->setContentsMargins(0, 0, 0, 0);
    m_wsLayout->setSpacing(Theme::rowSpacing);
    leftRow->addWidget(m_wsBox, 0, Qt::AlignVCenter);

    /* Focused window title, elided, fills the middle gap */
    m_title = new TextItem(m_leftBox);
    m_title->setColor(Theme::selected);
    m_title->setElide(true);
    leftRow->addWidget(m_title, 1, Qt::AlignVCenter);

    /* Centered clock: not part of any layout, positioned by relayoutBar(). */
    m_clock = new ClockWidget(this);

    /* Right-side pill cluster; auto-sizes via its own layout so
     * relayoutBar() can query sizeHint() and reposition it. */
    m_rightBox = new QWidget(this);
    auto *rightRow = new QHBoxLayout(m_rightBox);
    rightRow->setContentsMargins(0, 0, 0, 0);
    rightRow->setSpacing(Theme::rowSpacing);
    rightRow->addWidget(new NotificationWidget(m_rightBox), 0, Qt::AlignVCenter);
    /* Each of these shows only while it has something to say. */
    rightRow->addWidget(new ModesWidget(m_rightBox), 0, Qt::AlignVCenter);
    rightRow->addWidget(new UpdatesWidget(m_rightBox), 0, Qt::AlignVCenter);
    rightRow->addWidget(new DiskWidget(m_rightBox), 0, Qt::AlignVCenter);
    rightRow->addWidget(new LayoutWidget(screen, m_rightBox), 0, Qt::AlignVCenter);
    rightRow->addWidget(new NetworkWidget(m_rightBox), 0, Qt::AlignVCenter);
    /* Battery groups with the other status pills rather than sitting past the
     * tray, so the run of shell-owned indicators stays unbroken. Hidden
     * outright on a machine with no battery -- see battery.cpp. */
    rightRow->addWidget(new BatteryWidget(m_rightBox), 0, Qt::AlignVCenter);
    /* Same slot, opposite machine: exactly one of these is ever visible. */
    rightRow->addWidget(new PowerDrawWidget(m_rightBox), 0, Qt::AlignVCenter);
    /* VPN, Bluetooth, Clipboard live in the System dropdown (as larger tiles
     * alongside Weather) -- see system.cpp. */
    auto *tray = new TrayArea(m_rightBox);
    rightRow->addWidget(tray, 0, Qt::AlignVCenter);
    /* System furthest right, past the tray. */
    rightRow->addWidget(new SystemWidget(m_rightBox), 0, Qt::AlignVCenter);

    /* Either box's natural width can change after construction (tray icons
     * add/remove, a pill's label grows). Because these boxes are positioned
     * manually, a child first produces LayoutRequest rather than resizing its
     * parent; catch both that request and the resulting resize here. */
    m_rightBox->installEventFilter(this);
    m_clock->installEventFilter(this);

    /* Tray shows on this screen only if it is the tray host; re-evaluated on
     * monitor hot-plug. */
    auto updateTrayHost = [this, tray]() { tray->setHostsTray(hostsTray()); };
    updateTrayHost();
    connect(qApp, &QGuiApplication::screenAdded, tray, updateTrayHost);
    connect(qApp, &QGuiApplication::screenRemoved, tray, updateTrayHost);
    /* Which output counts as primary depends on the window manager's
     * monitor list, so it can settle after construction (first refresh) --
     * not just on Qt-side hotplug. */
    connect(m_state, &WmState::changed, tray, updateTrayHost);

    connect(m_state, &WmState::changed, this, &Panel::syncFromState);
    connect(m_state, &WmState::titleChanged, this,
            [this](const QString &monitorName) {
                if (m_screen && m_screen->name() == monitorName)
                    syncTitle();
            });

    /* Anchored to both side edges, the width follows the screen; Qt still
     * needs a size to lay widgets out against, and a mode change has to be
     * followed. */
    setFixedHeight(Theme::panelHeight);
    resize(screen ? screen->geometry().width() : 1920, Theme::panelHeight);
    if (screen) {
        connect(screen, &QScreen::geometryChanged, this,
                [this](const QRect &g) {
                    resize(g.width(), Theme::panelHeight);
                    Surface::setMargins(this, QMargins());
                    relayoutBar();
                });
    }

    syncFromState();
    relayoutBar();

    /* A dock across the top edge; the window manager reserves its height. */
    Surface::SurfaceSpec spec;
    spec.layer = Surface::Layer::Top;
    spec.anchors = Surface::AnchorTop | Surface::AnchorLeft | Surface::AnchorRight;
    /* Width 0 = fill between the anchored edges, i.e. the whole output. */
    spec.desiredSize = QSize(0, Theme::panelHeight);
    spec.exclusiveZone = Theme::panelHeight;
    spec.keyboard = Surface::Keyboard::None;
    Surface::configure(this, screen, spec);
}

/* The tray lives on the primary output -- see WmState::primaryMonitor().
 * Before the first state arrives there is nothing to compare against, so fall
 * back to the first screen Qt knows about rather than showing no tray at all. */
bool Panel::hostsTray() const
{
    if (!m_screen)
        return false;
    const QString primary = m_state->primaryMonitor();
    if (!primary.isEmpty())
        return m_screen->name() == primary;
    const QList<QScreen *> screens = QGuiApplication::screens();
    return !screens.isEmpty() && m_screen->name() == screens.first()->name();
}

/* WmState names each window-manager monitor after the QScreen with the same
 * geometry, so a panel finds its monitor by its own screen's name. */
const WmState::Monitor *Panel::mon() const
{
    if (!m_screen)
        return nullptr;
    if (const WmState::Monitor *m = m_state->monitorByName(m_screen->name()))
        return m;
    return m_state->monitors.isEmpty() ? nullptr : &m_state->monitors[0];
}

QString Panel::shortenTitle(const QString &t)
{
    const int max = maxTitleLength;
    if (t.length() <= max)
        return t;
    const QString ell = QStringLiteral("…");
    const QString sep = QStringLiteral(" - ");
    const int idx = t.lastIndexOf(sep);
    auto rstrip = [](QString s) {
        while (!s.isEmpty() && s.back().isSpace())
            s.chop(1);
        return s;
    };
    if (idx > 0) {
        const QString suffix = t.mid(idx); /* " - Mozilla Firefox" */
        const int budget = max - suffix.length() - ell.length();
        if (budget >= 1)
            return rstrip(t.left(budget)) + ell + suffix;
    }
    return rstrip(t.left(max - ell.length())) + ell;
}

void Panel::syncFromState()
{
    const WmState::Monitor *m = mon();
    const QStringList tags = m_state->workspaceNames;

    /* Rebuild only when the tag set changes (a config reload); otherwise the
     * row is fixed and we just restyle it. */
    if (m_wsTags != tags) {
        m_wsTags = tags;
        qDeleteAll(m_wsButtons);
        m_wsButtons.clear();
        for (int i = 0; i < tags.size(); i++) {
            auto *b = new WorkspaceButton(tags[i], m_wsBox);
            m_wsLayout->addWidget(b, 0, Qt::AlignVCenter);
            connect(b, &WorkspaceButton::clicked, this, [this, i]() {
                if (const WmState::Monitor *m = mon())
                    m_state->viewWorkspace(m->id, i);
            });
            /* dwm-style tags can be viewed together. */
            if (m_state->features().toggleWorkspaces)
                connect(b, &WorkspaceButton::rightClicked, this, [this, i]() {
                    if (const WmState::Monitor *m = mon())
                        m_state->toggleWorkspace(m->id, i);
                });
            m_wsButtons.push_back(b);
            b->show();
        }
    }
    for (int i = 0; i < m_wsButtons.size(); i++) {
        const quint32 bit = i < 32 ? (quint32(1) << i) : 0;
        m_wsButtons[i]->setStates(m && (m->selected & bit),
                                  m && (m->occupied & bit),
                                  m && (m->urgent & bit));
    }
    m_wsBox->setVisible(!m_wsButtons.isEmpty());

    syncTitle();
    relayoutBar();
}

void Panel::syncTitle()
{
    const WmState::Monitor *m = mon();
    const QString title = m ? m->title : QString();
    m_title->setText(title.isEmpty()
                         ? QString()
                         : QStringLiteral(" [ ") + shortenTitle(title)
                               + QStringLiteral(" ]"),
                     false /* elided title already owns fixed layout space */);
}

void Panel::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    relayoutBar();
}

bool Panel::eventFilter(QObject *watched, QEvent *event)
{
    const bool boxResized = event->type() == QEvent::Resize
                            && (watched == m_rightBox || watched == m_clock);
    const bool rightHintChanged = watched == m_rightBox
                                  && event->type() == QEvent::LayoutRequest;
    if (boxResized || rightHintChanged)
        relayoutBar();
    return QWidget::eventFilter(watched, event);
}

void Panel::relayoutBar()
{
    const int pad = Theme::panelPadding;
    const int gap = Theme::rowSpacing;

    const QSize rightSize = m_rightBox->sizeHint();
    const int rightX = width() - pad - rightSize.width();
    m_rightBox->setGeometry(rightX, 0, rightSize.width(), Theme::panelHeight);

    const QSize clockSize = m_clock->sizeHint();
    int clockX = (width() - clockSize.width()) / 2;
    clockX = qMin(clockX, rightX - gap - clockSize.width());
    clockX = qMax(clockX, pad);
    const int clockY = (Theme::panelHeight - clockSize.height()) / 2;
    m_clock->setGeometry(clockX, clockY, clockSize.width(),
                         clockSize.height());

    const int leftW = qMax(0, clockX - gap - pad);
    m_leftBox->setGeometry(pad, 0, leftW, Theme::panelHeight);
}

void Panel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QColor background = Theme::bg;
    background.setAlpha(Theme::panelOpacity);
    p.fillRect(rect(), background);
}
