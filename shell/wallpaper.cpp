#include "wallpaper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QWheelEvent>

#include <csignal>

#include "wmstate.h"
#include "paths.h"
#include "popup.h"
#include "theme.h"
#include "surface.h"

/* ---------------------------------------------------------- WallpaperPicker */

WallpaperPicker *WallpaperPicker::instance()
{
    static WallpaperPicker *s = new WallpaperPicker();
    return s;
}

QString WallpaperPicker::wallpaperDir()
{
    const QString env = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("FANSHELL_QS_WALLPAPER_DIR"));
    if (!env.isEmpty())
        return env;
    return QDir::homePath() + QStringLiteral("/Pictures/wallpapers");
}

QString WallpaperPicker::mode()
{
    static const QStringList modes{
        QStringLiteral("fill"), QStringLiteral("center"), QStringLiteral("max"),
        QStringLiteral("scale"), QStringLiteral("tile")};
    const QString env = QProcessEnvironment::systemEnvironment()
                            .value(QStringLiteral("FANSHELL_QS_WALLPAPER_MODE"))
                            .trimmed()
                            .toLower();
    return modes.contains(env) ? env : QStringLiteral("fill");
}

WallpaperPicker::WallpaperPicker(QObject *parent) : QObject(parent)
{
    m_win = new WallpaperWindow(this);

    m_decoder.setSingleShot(false);
    m_decoder.setInterval(0);
    connect(&m_decoder, &QTimer::timeout, this, &WallpaperPicker::decodeNext);

    rescan();
    refreshCurrent();
    restore();
}

void WallpaperPicker::rescan()
{
    /* Deliberately not recursive: see wallpaper.h. */
    QDir dir(wallpaperDir());
    const QStringList globs{
        QStringLiteral("*.png"),  QStringLiteral("*.jpg"),
        QStringLiteral("*.jpeg"), QStringLiteral("*.webp"),
        QStringLiteral("*.gif"),  QStringLiteral("*.bmp")};
    const QFileInfoList files =
        dir.entryInfoList(globs, QDir::Files | QDir::Readable, QDir::Name);

    /* Keep already-decoded thumbnails across a rescan. */
    QHash<QString, QPixmap> keep;
    for (const Wallpaper &w : std::as_const(entries))
        if (w.decoded && !w.thumb.isNull())
            keep.insert(w.path, w.thumb);

    entries.clear();
    for (const QFileInfo &fi : files) {
        Wallpaper w;
        w.path = fi.absoluteFilePath();
        w.name = fi.completeBaseName();
        const auto it = keep.constFind(w.path);
        if (it != keep.constEnd()) {
            w.thumb = *it;
            w.decoded = true;
        }
        entries.push_back(w);
    }
    if (selected >= entries.size())
        selected = qMax(0, entries.size() - 1);

    bool pending = false;
    for (const Wallpaper &w : std::as_const(entries))
        pending = pending || !w.decoded;
    if (pending)
        m_decoder.start();

    emit changed();
}

void WallpaperPicker::decodeNext()
{
    for (Wallpaper &w : entries) {
        if (w.decoded)
            continue;
        QImageReader r(w.path);
        r.setAutoTransform(true);
        /* Ask the decoder itself to downscale -- libjpeg/libpng can do this
         * while reading, which is far cheaper than decoding full size and
         * scaling after (12MB PNGs here). */
        QSize s = r.size();
        if (s.isValid()) {
            s.scale(thumbW, thumbH, Qt::KeepAspectRatioByExpanding);
            r.setScaledSize(s);
        }
        const QImage img = r.read();
        if (!img.isNull()) {
            /* Cover-crop to the cell so the grid stays a tidy lattice
             * regardless of each wallpaper's aspect ratio. */
            const QImage scaled = img.scaled(thumbW, thumbH,
                                             Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation);
            const int dx = (scaled.width() - thumbW) / 2;
            const int dy = (scaled.height() - thumbH) / 2;
            w.thumb = QPixmap::fromImage(
                scaled.copy(dx, dy, thumbW, thumbH));
        }
        w.decoded = true;
        emit changed();
        return;
    }
    m_decoder.stop();
}

/* The path the last feh-written ~/.fehbg shows, for a one-time import. It is
 * a shell script whose last word is the image:
 *     feh --no-fehbg --bg-fill '/path/to/file.jpg' */
static QString fehbgPath()
{
    QFile file(QDir::homePath() + QStringLiteral("/.fehbg"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QString path;
    for (const QString &raw : QString::fromUtf8(file.readAll()).split('\n')) {
        const QString line = raw.trimmed();
        if (!line.contains(QLatin1String("feh")) || !line.endsWith('\''))
            continue;
        const int open = line.lastIndexOf('\'', line.size() - 2);
        if (open >= 0)
            path = line.mid(open + 1, line.size() - open - 2);
    }
    return path;
}

void WallpaperPicker::save(const QString &path, const QString &feMode)
{
    if (!Paths::writeConf(QLatin1String(stateName),
                          QStringLiteral("Wallpaper set from the picker; the "
                                         "shell restores it at startup."),
                          {QStringLiteral("path=") + path,
                           QStringLiteral("mode=") + feMode}))
        qWarning("fanshell-qs: cannot write %s",
                 qPrintable(Paths::configFile(QLatin1String(stateName))));
}

void WallpaperPicker::refreshCurrent()
{
    /* Every output carries the same wallpaper, so one path is enough to mark
     * the active cell. */
    QString path = Paths::readConf(QLatin1String(stateName))
                       .value(QStringLiteral("path"));
    if (path.isEmpty()) {
        /* First run since the state moved out of ~/.fehbg: carry it over. */
        path = fehbgPath();
        if (!path.isEmpty())
            save(path, mode());
    }
    if (path.isEmpty() || current == path)
        return;
    current = path;
    /* Park the cursor on whatever is actually displayed, so opening the
     * picker starts from where you are. Only while closed -- never yank a
     * selection the user is in the middle of moving. */
    if (!m_win->isVisible())
        syncSelectionToCurrent();
    emit changed();
}

void WallpaperPicker::runFeh(const QString &path, const QString &feMode,
                             bool remember)
{
    /* feh sets every output. --no-fehbg: the choice is kept in
     * wallpaper.conf, not in a script feh would drop into $HOME. */
    auto *proc = new QProcess(this);
    proc->setProgram(QStringLiteral("feh"));
    proc->setArguments({QStringLiteral("--no-fehbg"),
                        QStringLiteral("--bg-") + feMode, path});
    proc->setChildProcessModifier([]() {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
    });
    connect(proc, &QProcess::finished, this,
            [this, proc, path, feMode, remember]() {
        if (proc->exitStatus() == QProcess::NormalExit
                && proc->exitCode() == 0) {
            if (remember)
                save(path, feMode);
            current = path;
            emit changed();
        } else {
            qWarning("fanshell-qs: feh --bg-%s failed (%s)",
                     qPrintable(feMode),
                     qPrintable(proc->readAllStandardError().trimmed()));
        }
        proc->deleteLater();
    });
    proc->start();
}

void WallpaperPicker::apply(int index)
{
    if (index < 0 || index >= entries.size())
        return;
    runFeh(entries[index].path, mode(), true);
}

void WallpaperPicker::restore()
{
    const QHash<QString, QString> conf =
        Paths::readConf(QLatin1String(stateName));
    const QString path = conf.value(QStringLiteral("path"), current);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    /* The mode it was set with, so a later env change does not silently
     * re-crop what is already chosen. */
    QString feMode = conf.value(QStringLiteral("mode"));
    if (feMode.isEmpty())
        feMode = mode();
    runFeh(path, feMode, false);
}

void WallpaperPicker::syncSelectionToCurrent()
{
    for (int i = 0; i < entries.size(); i++)
        if (entries[i].path == current) {
            selected = i;
            return;
        }
}

void WallpaperPicker::move(int delta)
{
    if (entries.isEmpty())
        return;
    selected = qBound(0, selected + delta, entries.size() - 1);
    emit selectionChanged();
}

void WallpaperPicker::openPicker()
{
    /* Pick up anything added to the directory since the last open, and
     * re-read what wallpaper.conf says is showing. */
    rescan();
    refreshCurrent();
    syncSelectionToCurrent();
    /* openOn() resolves the focused output itself. */
    m_win->openOn(nullptr);
}

void WallpaperPicker::togglePicker()
{
    if (m_win->isVisible())
        hidePicker();
    else
        openPicker();
}

void WallpaperPicker::hidePicker()
{
    m_win->hide();
}

/* ------------------------------------------------------------ WallpaperGrid */

WallpaperGrid::WallpaperGrid(WallpaperPicker *p, QWidget *parent)
    : QWidget(parent), m_p(p)
{
    setMouseTracking(true);
    connect(p, &WallpaperPicker::changed, this, [this]() { update(); });
    connect(p, &WallpaperPicker::selectionChanged, this, [this]() {
        ensureVisible();
        update();
    });
}

int WallpaperGrid::contentHeight() const
{
    const int n = m_p->entries.size();
    if (n == 0)
        return 0;
    const int rows = (n + WallpaperPicker::columns - 1) / WallpaperPicker::columns;
    return rows * WallpaperPicker::cellH();
}

void WallpaperGrid::clampScroll()
{
    m_scroll = qBound(0, m_scroll, qMax(0, contentHeight() - height()));
}

void WallpaperGrid::ensureVisible()
{
    const int row = m_p->selected / WallpaperPicker::columns;
    const int top = row * WallpaperPicker::cellH();
    const int bottom = top + WallpaperPicker::cellH();
    if (top < m_scroll)
        m_scroll = top;
    else if (bottom > m_scroll + height())
        m_scroll = bottom - height();
    clampScroll();
}

int WallpaperGrid::indexAt(const QPoint &pt) const
{
    if (pt.x() < 0 || pt.x() >= WallpaperPicker::columns * WallpaperPicker::cellW())
        return -1;
    const int col = pt.x() / WallpaperPicker::cellW();
    const int row = (pt.y() + m_scroll) / WallpaperPicker::cellH();
    const int i = row * WallpaperPicker::columns + col;
    return (i >= 0 && i < m_p->entries.size()) ? i : -1;
}

void WallpaperGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int cw = WallpaperPicker::cellW();
    const int ch = WallpaperPicker::cellH();
    const int pad = WallpaperPicker::cellPad;

    for (int i = 0; i < m_p->entries.size(); i++) {
        const int row = i / WallpaperPicker::columns;
        const int col = i % WallpaperPicker::columns;
        const QRect cell(col * cw, row * ch - m_scroll, cw, ch);
        if (!cell.intersects(rect()))
            continue;

        const Wallpaper &w = m_p->entries[i];
        const bool sel = (i == m_p->selected);
        const bool active = (w.path == m_p->current);

        if (sel)
            Theme::paintRect(p, cell.adjusted(2, 2, -2, -2), Theme::surface,
                             Theme::radius);

        const QRect thumb(cell.x() + pad, cell.y() + pad,
                          WallpaperPicker::thumbW, WallpaperPicker::thumbH);
        if (!w.thumb.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(thumb, Theme::smallRadius, Theme::smallRadius);
            p.save();
            p.setClipPath(clip);
            p.drawPixmap(thumb, w.thumb);
            p.restore();
        } else {
            /* Still decoding, or unreadable. */
            Theme::paintRect(p, thumb, Theme::surfaceHover, Theme::smallRadius);
            p.setPen(Theme::textMuted);
            p.setFont(Theme::font(Theme::smallFontSize));
            p.drawText(thumb, Qt::AlignCenter,
                       w.decoded ? QStringLiteral("unreadable")
                                 : QStringLiteral("…"));
        }

        /* The wallpaper in use gets an accent frame; the keyboard cursor gets
         * the surface fill above. They stack, so you can tell "selected" from
         * "currently applied" at a glance. */
        if (active)
            Theme::paintRect(p, thumb, Theme::transparent, Theme::smallRadius,
                             Theme::accent, 2);

        const QRect label(cell.x() + pad, thumb.bottom() + 1 + pad,
                          WallpaperPicker::thumbW, WallpaperPicker::labelH);
        p.setFont(Theme::font(Theme::smallFontSize, sel || active));
        p.setPen(active ? Theme::accent : (sel ? Theme::text : Theme::textMuted));
        const QFontMetrics fm(p.font());
        p.drawText(label, Qt::AlignHCenter | Qt::AlignVCenter,
                   fm.elidedText(w.name, Qt::ElideMiddle, label.width()));
    }
}

void WallpaperGrid::mouseMoveEvent(QMouseEvent *e)
{
    const int i = indexAt(e->pos());
    if (i >= 0 && i != m_p->selected) {
        m_p->selected = i;
        update();
    }
}

void WallpaperGrid::mousePressEvent(QMouseEvent *e)
{
    const int i = indexAt(e->pos());
    if (i < 0)
        return;
    m_p->selected = i;
    m_p->apply(i);
    m_p->hidePicker();
}

void WallpaperGrid::wheelEvent(QWheelEvent *e)
{
    m_scroll -= e->angleDelta().y();
    clampScroll();
    update();
}

void WallpaperGrid::resizeEvent(QResizeEvent *)
{
    clampScroll();
}

/* ---------------------------------------------------------- WallpaperWindow */

namespace {

QRect boxRectFor(const QSize &winSize, int gridContentH)
{
    const int w = WallpaperPicker::columns * WallpaperPicker::cellW()
                  + 2 * Theme::popupMargin;
    const int maxH = qRound(winSize.height() * 0.72);
    const int h = qMin(maxH, 2 * Theme::popupMargin + gridContentH);
    const int x = (winSize.width() - w) / 2;
    const int y = (winSize.height() - h) / 2;
    return QRect(x, y, w, h);
}

} /* namespace */

WallpaperWindow::WallpaperWindow(WallpaperPicker *p)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), m_p(p)
{
    setWindowTitle(QStringLiteral("fanshell-wallpaper"));
    setAttribute(Qt::WA_TranslucentBackground);
    m_grid = new WallpaperGrid(p, this);
    connect(p, &WallpaperPicker::changed, this, [this]() {
        if (isVisible())
            layoutBox();
    });
}

void WallpaperWindow::openOn(QScreen *screen)
{
    /* An open popup holds keyboard focus; drop it before we take over. */
    PopupManager::closeCurrent();

    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QString focused = WmState::instance()->focusedMonitor();
    if (!focused.isEmpty())
        for (QScreen *s : QGuiApplication::screens())
            if (s->name() == focused)
                screen = s;
    if (!screen)
        return;

    if (m_configured && m_screen != screen) {
        /* Following the focused monitor: re-place on the new one. */
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
        spec.scope = QStringLiteral("fanshell-qs-wallpaper");
        Surface::configure(this, screen, spec);
        m_screen = screen;
        m_configured = true;
    }
    show();
    layoutBox();
    m_grid->ensureVisible();
    setFocus(Qt::OtherFocusReason);
}

void WallpaperWindow::layoutBox()
{
    const QRect box = boxRectFor(size(), m_grid->contentHeight());
    m_grid->setGeometry(box.adjusted(Theme::popupMargin, Theme::popupMargin,
                                     -Theme::popupMargin,
                                     -Theme::popupMargin));
    update();
}

void WallpaperWindow::resizeEvent(QResizeEvent *)
{
    layoutBox();
}

void WallpaperWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect box = boxRectFor(size(), m_grid->contentHeight());
    Theme::paintRect(p, box, Theme::bg, Theme::radius, Theme::border, 1);
    if (m_p->entries.isEmpty()) {
        p.setPen(Theme::textMuted);
        p.setFont(Theme::font(Theme::bodyFontSize));
        p.drawText(box, Qt::AlignCenter,
                   QStringLiteral("No images in %1")
                       .arg(WallpaperPicker::wallpaperDir()));
    }
}

void WallpaperWindow::mousePressEvent(QMouseEvent *e)
{
    /* Click on the transparent area outside the box dismisses; clicks inside
     * are the grid's business. */
    if (!boxRectFor(size(), m_grid->contentHeight()).contains(e->pos()))
        m_p->hidePicker();
}

void WallpaperWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Escape:
        m_p->hidePicker();
        return;
    case Qt::Key_Left:
        m_p->move(-1);
        return;
    case Qt::Key_Right:
        m_p->move(1);
        return;
    case Qt::Key_Up:
        m_p->move(-WallpaperPicker::columns);
        return;
    case Qt::Key_Down:
        m_p->move(WallpaperPicker::columns);
        return;
    case Qt::Key_Home:
        m_p->move(-m_p->entries.size());
        return;
    case Qt::Key_End:
        m_p->move(m_p->entries.size());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        m_p->apply(m_p->selected);
        m_p->hidePicker();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}
