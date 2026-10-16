#include "launcher.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QTimer>
#include <QWheelEvent>

#include <csignal>

#include "wmstate.h"
#include "emoji_data.h"
#include "popup.h"
#include "surface.h"

/* ------------------------------------------------------------- AppLauncher */

AppLauncher::AppLauncher(QObject *parent) : QObject(parent)
{
    const QStringList emojiLines =
        QString::fromUtf8(emojiData).split(QLatin1Char('\n'),
                                           Qt::SkipEmptyParts);
    emojis.reserve(emojiLines.size());
    for (const QString &line : emojiLines) {
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() >= 3)
            emojis.push_back({fields[0], fields[1], fields[2]});
    }

    /* Enumerate executables on $PATH for run mode. */
    m_binScan.setCommand(
        {"sh", "-c",
         "for d in $(echo \"$PATH\" | tr ':' ' '); do [ -d \"$d\" ] || "
         "continue; for f in \"$d\"/*; do [ -x \"$f\" ] && [ ! -d \"$f\" ] && "
         "printf '%s\\n' \"${f##*/}\"; done; done | sort -u"});
    connect(&m_binScan, &CollectorProcess::finished, this,
            [this](const QString &text) {
                binaries.clear();
                const QStringList lines = text.split('\n');
                for (const QString &l : lines)
                    if (!l.isEmpty())
                        binaries << l;
                if (m_win->isVisible() && mode == QLatin1String("run"))
                    refilter();
            });

    connect(DesktopEntries::instance(), &DesktopEntries::valuesChanged, this,
            [this]() {
                if (m_win->isVisible() && mode == QLatin1String("apps"))
                    refilter();
            });

    m_win = new LauncherWindow(this);
}

QScreen *AppLauncher::pickScreen() const
{
    /* Wherever the user currently is -- not the primary output. Re-read on
     * every open, so the launcher follows focus between monitors. */
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QString focused = WmState::instance()->focusedMonitor();
    if (!focused.isEmpty())
        for (QScreen *s : screens)
            if (s->name() == focused)
                return s;
    return screens.isEmpty() ? nullptr : screens.first();
}

void AppLauncher::refilter()
{
    /* A live DesktopEntries rescan (new/removed .desktop files -- e.g. an
     * AppImage integration daemon rewriting the applications dir right
     * around login) re-triggers this while the launcher is open and the
     * user already has something highlighted. Resetting to entries[0]
     * unconditionally would silently retarget whatever Enter/click is about
     * to launch, with no visible sign the highlight moved. Preserve the
     * selection by stable identity (desktop-file id / binary name, not
     * position) across the rebuild when it's still present. */
    QString keepId, keepCmd;
    if (selected >= 0 && selected < entries.size()) {
        if (entries[selected].isApp)
            keepId = entries[selected].app.id;
        else
            keepCmd = entries[selected].cmd;
    }

    const QString q = searchText.toLower().trimmed();
    QVector<LauncherItem> out;
    if (mode == QLatin1String("menu")) {
        for (const LauncherItem &item : menuItems())
            if (q.isEmpty() || (item.name + QLatin1Char(' ') + item.sub)
                                  .toLower().contains(q))
                out.push_back(item);
    } else if (mode == QLatin1String("emoji")) {
        const QStringList terms = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const EmojiEntry &emoji : emojis) {
            const QString haystack =
                emoji.value + QLatin1Char(' ') + emoji.name.toLower()
                + QLatin1Char(' ')
                + emoji.keywords.toLower();
            bool matches = true;
            for (const QString &term : terms)
                if (!haystack.contains(term)) {
                    matches = false;
                    break;
                }
            if (!matches)
                continue;
            LauncherItem it;
            it.name = emoji.name;
            it.sub = emoji.keywords;
            it.icon = emoji.value;
            it.cmd = emoji.value;
            it.isEmoji = true;
            out.push_back(it);
        }
    } else if (mode == QLatin1String("run")) {
        for (const QString &b : binaries) {
            if (q.isEmpty() || b.toLower().contains(q)) {
                LauncherItem it;
                it.name = b;
                it.icon = b;
                it.cmd = b;
                out.push_back(it);
            }
        }
        /* bins arrive pre-sorted (sort -u) */
    } else {
        const QVector<DesktopEntry> &all =
            DesktopEntries::instance()->applications();
        for (int i = 0; i < all.size(); i++) {
            const DesktopEntry &e = all[i];
            if (e.noDisplay)
                continue;
            if (q.isEmpty() || e.name.toLower().contains(q)
                    || e.genericName.toLower().contains(q)
                    || e.comment.toLower().contains(q)) {
                LauncherItem it;
                it.name = e.name;
                it.sub = e.genericName.isEmpty() ? e.comment : e.genericName;
                it.icon = e.icon;
                it.isApp = true;
                it.app = e;
                out.push_back(it);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const LauncherItem &a, const LauncherItem &b) {
                      return QString::localeAwareCompare(a.name.toLower(),
                                                         b.name.toLower())
                             < 0;
                  });
    }
    entries = out;
    selected = 0;
    if (!keepId.isEmpty() || !keepCmd.isEmpty()) {
        for (int i = 0; i < entries.size(); i++) {
            if ((!keepId.isEmpty() && entries[i].isApp
                        && entries[i].app.id == keepId)
                    || (!keepCmd.isEmpty() && !entries[i].isApp
                        && entries[i].cmd == keepCmd)) {
                selected = i;
                break;
            }
        }
    }
    emit entriesChanged();
    emit selectedChanged();
}

void AppLauncher::launchSelected()
{
    /* Close the launcher before launching, not after -- harmless either
     * way, kept for AppImage entries: some AppImages fork+waitpid() their
     * own FUSE-mounting helper at startup, and closing our fullscreen
     * overlay first avoids at least one window of overlap with that.
     * NOTE: this alone is not a confirmed fix for AppImage launches
     * failing intermittently after fanshell-qs-shell has been running a while --
     * see DesktopEntry::execute()'s child-process signal reset, which
     * targets the actual mechanism found so far. `it` is a value copy, not
     * a reference into `entries`: hideLauncher() pumps the event loop for
     * the unmap round-trip, and a DesktopEntries rescan landing in that
     * window could otherwise reallocate the vector out from under a
     * dangling reference. */
    if (selected >= 0 && selected < entries.size()) {
        LauncherItem it = entries[selected];
        if (!it.menuAction.isEmpty()) {
            activateMenuItem(it.menuAction);
            return;
        }
        hideLauncher();
        if (it.isApp) {
            it.app.execute();
        } else if (it.isEmoji) {
            /* The shell owns the selection from here on, for as long as it
             * runs. */
            QGuiApplication::clipboard()->setText(it.cmd, QClipboard::Clipboard);
        } else if (!it.cmd.isEmpty()) {
            auto *proc = new QProcess;
            proc->setProgram(it.cmd);
            proc->setChildProcessModifier([]() {
                signal(SIGCHLD, SIG_DFL);
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
            });
            proc->startDetached();
            proc->deleteLater();
        }
        return;
    }
    hideLauncher();
}

void AppLauncher::move(int delta)
{
    if (entries.isEmpty())
        return;
    selected = (selected + delta + entries.size()) % entries.size();
    emit selectedChanged();
}

void AppLauncher::movePage(int delta)
{
    if (entries.isEmpty())
        return;
    selected = qBound(0, selected + delta, entries.size() - 1);
    emit selectedChanged();
}

void AppLauncher::openMode(const QString &m)
{
    mode = m;
    if (m == QLatin1String("menu"))
        menuPage = QStringLiteral("main");
    searchText.clear();
    if (m == QLatin1String("run"))
        m_binScan.start(); /* (re)scan $PATH; refilter again when it ends */
    refilter();
    m_win->openOn(pickScreen());
}

void AppLauncher::toggleMode(const QString &m)
{
    if (m_win->isVisible() && mode == m)
        hideLauncher();
    else
        openMode(m);
}

void AppLauncher::hideLauncher()
{
    m_win->hide();
}

void AppLauncher::scanBinaries()
{
    m_binScan.start();
}

/* ------------------------------------------------------------ LauncherGrid */

LauncherGrid::LauncherGrid(AppLauncher *l, QWidget *parent)
    : QWidget(parent), m_l(l)
{
    setMouseTracking(true);
    connect(l, &AppLauncher::entriesChanged, this, [this]() {
        m_scroll = 0;
        clampScroll();
        update();
    });
    connect(l, &AppLauncher::selectedChanged, this, [this]() {
        ensureVisible();
        update();
    });
}

int LauncherGrid::contentHeight() const
{
    const int rows =
        (m_l->entries.size() + m_l->columnCount() - 1) / m_l->columnCount();
    return rows * 48;
}

int LauncherGrid::pageStep() const
{
    /* Advance by whole visible rows and keep the selected column stable. */
    return qMax(1, height() / 48) * m_l->columnCount();
}

void LauncherGrid::clampScroll()
{
    m_scroll = qMax(0, qMin(m_scroll, contentHeight() - height()));
}

void LauncherGrid::ensureVisible()
{
    if (height() <= 0)
        return; /* not laid out yet; resizeEvent re-runs this */
    const int row = m_l->selected / m_l->columnCount();
    const int top = row * 48;
    if (top - m_scroll < 0)
        m_scroll = top;
    else if (top + 48 - m_scroll > height())
        m_scroll = top + 48 - height();
    clampScroll();
}

int LauncherGrid::indexAt(const QPoint &p) const
{
    const int cw = cellWidth();
    if (cw <= 0)
        return -1;
    const int col = qMin(p.x() / cw, m_l->columnCount() - 1);
    const int row = (p.y() + m_scroll) / 48;
    const int idx = row * m_l->columnCount() + col;
    return (idx >= 0 && idx < m_l->entries.size()) ? idx : -1;
}

void LauncherGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int cw = cellWidth();
    const int first = (m_scroll / 48) * m_l->columnCount();
    const int last = qMin(m_l->entries.size() - 1,
                          ((m_scroll + height()) / 48 + 1)
                              * m_l->columnCount());
    for (int i = qMax(0, first); i <= last; i++) {
        const int row = i / m_l->columnCount();
        const int col = i % m_l->columnCount();
        const QRect cell(col * cw, row * 48 - m_scroll, cw, 48);
        /* Inset from the cell edges (compactSpacing / 2). */
        const QRect inner = cell.adjusted(1, 1, -1, -1);
        const bool sel = (i == m_l->selected);
        if (sel)
            Theme::paintRect(p, inner, Theme::surface, Theme::smallRadius);

        const LauncherItem &it = m_l->entries[i];
        /* icon 24x24, row leftMargin 8 */
        const QRect iconRect(inner.x() + Theme::rowSpacing,
                             inner.y() + (inner.height() - 24) / 2, 24, 24);
        if (it.isEmoji) {
            QFont emojiFont(QStringLiteral("Noto Color Emoji"));
            emojiFont.setPixelSize(22);
            p.setFont(emojiFont);
            p.setPen(Theme::textStrong);
            p.drawText(iconRect.adjusted(-2, -2, 4, 4),
                       Qt::AlignCenter, it.icon);
        } else {
            QIcon icon = QFileInfo::exists(it.icon) ? QIcon(it.icon)
                                                   : QIcon::fromTheme(it.icon);
            if (icon.isNull() && it.menuAction.startsWith(QLatin1String("url:")))
                icon = QIcon::fromTheme(QStringLiteral("web-browser"));
            if (icon.isNull())
                icon = QIcon::fromTheme(
                    QStringLiteral("application-x-executable"));
            icon.paint(&p, iconRect);
        }

        const int textX = iconRect.right() + 1 + Theme::rowSpacing;
        const int textW = inner.right() + 1 - Theme::rowSpacing - textX;
        const QFontMetrics fmB(Theme::font(Theme::bodyFontSize));
        const QFontMetrics fmS(Theme::font(Theme::smallFontSize));
        const bool hasSub = !it.sub.isEmpty();
        const int colH = fmB.height() + (hasSub ? fmS.height() : 0);
        int y = inner.y() + (inner.height() - colH) / 2;
        p.setFont(Theme::font(Theme::bodyFontSize));
        p.setPen(sel ? Theme::cyan : Theme::text);
        p.drawText(QRect(textX, y, textW, fmB.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fmB.elidedText(it.name, Qt::ElideRight, textW));
        if (hasSub) {
            y += fmB.height();
            p.setFont(Theme::font(Theme::smallFontSize));
            p.setPen(Theme::textMuted);
            p.drawText(QRect(textX, y, textW, fmS.height()),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fmS.elidedText(it.sub, Qt::ElideRight, textW));
        }
    }
}

void LauncherGrid::mouseMoveEvent(QMouseEvent *e)
{
    const int idx = indexAt(e->pos());
    if (idx >= 0 && idx != m_l->selected) {
        m_l->selected = idx;
        /* hover selection must not scroll the view */
        update();
    }
}

void LauncherGrid::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const int idx = indexAt(e->pos());
    if (idx >= 0) {
        m_l->selected = idx;
        m_l->launchSelected();
    }
}

void LauncherGrid::wheelEvent(QWheelEvent *e)
{
    m_scroll -= e->angleDelta().y();
    clampScroll();
    update();
}

void LauncherGrid::resizeEvent(QResizeEvent *)
{
    clampScroll();
    ensureVisible();
}

/* ---------------------------------------------------------- LauncherWindow */

LauncherWindow::LauncherWindow(AppLauncher *l)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), m_l(l)
{
    setWindowTitle(QStringLiteral("fanshell-launcher"));
    setAttribute(Qt::WA_TranslucentBackground);

    m_search = new QLineEdit(this);
    m_search->setFrame(false);
    m_search->setFont(Theme::font(Theme::bodyFontSize));
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit { background: transparent; border: none; color: %1; }")
                                .arg(Theme::text.name()));
    QPalette pal = m_search->palette();
    pal.setColor(QPalette::PlaceholderText, Theme::textMuted);
    pal.setColor(QPalette::Highlight, Theme::cyan);
    m_search->setPalette(pal);
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_l->searchText = t;
        m_l->refilter();
    });

    m_grid = new LauncherGrid(l, this);

    connect(l, &AppLauncher::entriesChanged, this, [this]() {
        if (isVisible())
            layoutBox();
    });
}

static QRect boxRectFor(const QSize &winSize, int gridContentH, bool centered)
{
    const int w = qMin(centered ? 560 : 820, qMax(1, winSize.width() - 28));
    const int h = qMin(520, 2 * Theme::popupMargin + Theme::buttonHeight
                                + Theme::listSpacing + gridContentH);
    const int x = (winSize.width() - w) / 2;
    const int y = centered ? (winSize.height() - h) / 2
                           : qRound(winSize.height() * 0.28);
    return QRect(x, y, w, h);
}

void LauncherWindow::layoutBox()
{
    const QRect box = boxRectFor(size(), m_grid->contentHeight(),
                                m_l->mode == QLatin1String("menu"));
    /* search bar: box margins 14, height 28 */
    const QRect bar(box.x() + Theme::popupMargin, box.y() + Theme::popupMargin,
                    box.width() - 2 * Theme::popupMargin, Theme::buttonHeight);
    /* row inside the bar: l/r margins 14, glyph (width 0) + spacing 8 */
    const QFontMetrics fm(m_search->font());
    const int rowW = bar.width() - 2 * Theme::popupMargin;
    const int inputW = rowW - 40;
    const int inputH = fm.height() + 2;
    m_search->setGeometry(bar.x() + Theme::popupMargin + Theme::rowSpacing,
                          bar.y() + (bar.height() - inputH) / 2, inputW,
                          inputH);
    /* grid: margins 14, topMargin 4 under the header */
    const int gridY = bar.bottom() + 1 + Theme::listSpacing;
    m_grid->setGeometry(box.x() + Theme::popupMargin, gridY,
                        box.width() - 2 * Theme::popupMargin,
                        box.bottom() + 1 - Theme::popupMargin - gridY);
    update();
}

void LauncherWindow::openOn(QScreen *screen)
{
    /* An open popup holds the X input focus; if one is up when the launcher
     * appears, it would take it back on close. Drop it before taking focus. */
    PopupManager::closeCurrent();

    m_search->blockSignals(true);
    m_search->clear();
    m_search->blockSignals(false);
    m_search->setPlaceholderText(
        m_l->mode == QLatin1String("menu")
            ? m_l->menuTitle() + QStringLiteral(" — search actions…")
            : m_l->mode == QLatin1String("run")
            ? QStringLiteral("Run a command…")
            : (m_l->mode == QLatin1String("emoji")
                   ? QStringLiteral("Search emoji names…")
                   : QStringLiteral("Search applications…")));
    /* A fullscreen override-redirect window raised above the bar and every
     * popup, grabbing the keyboard while shown -- so, unlike the old X11
     * build, config.lua needs no window rule to float and focus it.
     * exclusiveZone -1 keeps the bar's reserved strip from pushing us down,
     * so the dim really does cover the whole output. */
    if (screen) {
        if (m_configured && m_screen != screen) {
            /* Following the focused output: re-place on the new one. */
            hide();
            m_configured = false;
        }
        resize(screen->geometry().size());
        if (!m_configured) {
            Surface::SurfaceSpec spec;
            spec.layer = Surface::Layer::Overlay;
            spec.anchors = Surface::AnchorTop | Surface::AnchorBottom
                           | Surface::AnchorLeft | Surface::AnchorRight;
            spec.desiredSize = QSize(0, 0); /* fill the output */
            spec.exclusiveZone = -1;
            spec.keyboard = Surface::Keyboard::Exclusive;
            spec.type = Surface::WindowType::Dialog;
            spec.scope = QStringLiteral("fanshell-qs-launcher");
            Surface::configure(this, screen, spec);
            m_screen = screen;
            m_configured = true;
        }
    }
    show();
    layoutBox();
}

void LauncherWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    /* Hand focus to the search field once the window has mapped and taken
     * the keyboard. */
    QTimer::singleShot(0, m_search,
                       [this]() { m_search->setFocus(Qt::OtherFocusReason); });
}

void LauncherWindow::resizeEvent(QResizeEvent *)
{
    layoutBox();
}

void LauncherWindow::mousePressEvent(QMouseEvent *e)
{
    /* Click on the (transparent) dim outside the box closes the launcher;
     * clicks inside the box are swallowed. */
    const QRect box = boxRectFor(size(), m_grid->contentHeight(),
                                m_l->mode == QLatin1String("menu"));
    if (!box.contains(e->pos()))
        m_l->hideLauncher();
}

bool LauncherWindow::handleKey(QKeyEvent *k)
{
    const bool ctrl = k->modifiers() & Qt::ControlModifier;
    if (k->key() == Qt::Key_Escape) {
        if (m_l->mode == QLatin1String("menu")
                && m_l->menuPage != QLatin1String("main"))
            m_l->openMenuPage(QStringLiteral("main"));
        else
            m_l->hideLauncher();
        return true;
    }
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
        m_l->launchSelected();
        return true;
    }
    if (k->key() == Qt::Key_Down || (k->key() == Qt::Key_N && ctrl)) {
        m_l->move(m_l->columnCount());
        return true;
    }
    if (k->key() == Qt::Key_Up || (k->key() == Qt::Key_P && ctrl)) {
        m_l->move(-m_l->columnCount());
        return true;
    }
    if (k->key() == Qt::Key_Right || (k->key() == Qt::Key_F && ctrl)) {
        m_l->move(1);
        return true;
    }
    if (k->key() == Qt::Key_Left || (k->key() == Qt::Key_B && ctrl)) {
        m_l->move(-1);
        return true;
    }
    if (k->key() == Qt::Key_PageDown) {
        m_l->movePage(m_grid->pageStep());
        return true;
    }
    if (k->key() == Qt::Key_PageUp) {
        m_l->movePage(-m_grid->pageStep());
        return true;
    }
    return false;
}

bool LauncherWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_search && e->type() == QEvent::KeyPress
            && handleKey(static_cast<QKeyEvent *>(e)))
        return true;
    return QWidget::eventFilter(o, e);
}

/* Fallback for keys that land on the window itself (focus not yet handed to
 * the search field, e.g. right after the surface maps): action keys
 * work regardless, and printable input is forwarded into the field. */
void LauncherWindow::keyPressEvent(QKeyEvent *k)
{
    if (handleKey(k))
        return;
    if (!k->text().isEmpty() && m_search && !m_search->hasFocus()) {
        m_search->setFocus(Qt::OtherFocusReason);
        QApplication::sendEvent(m_search, k);
        return;
    }
    QWidget::keyPressEvent(k);
}

bool LauncherWindow::event(QEvent *e)
{
    /* Re-assert search focus whenever the WM activates us. */
    if (e->type() == QEvent::WindowActivate && m_search)
        m_search->setFocus(Qt::OtherFocusReason);
    return QWidget::event(e);
}

void LauncherWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    /* No dim: fully transparent overlay; only the box is visible. */
    const QRect box = boxRectFor(size(), m_grid->contentHeight(),
                                m_l->mode == QLatin1String("menu"));
    Theme::paintRect(p, box, Theme::bg, Theme::radius, Theme::border, 1);
    const QRect bar(box.x() + Theme::popupMargin, box.y() + Theme::popupMargin,
                    box.width() - 2 * Theme::popupMargin, Theme::buttonHeight);
    Theme::paintRect(p, bar, Theme::surface, Theme::smallRadius);
    /* search glyph slot (empty string in the current theme) renders nothing */
}
