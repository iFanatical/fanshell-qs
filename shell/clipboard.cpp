#include "clipboard.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QMimeData>
#include <QProcess>
#include <QStandardPaths>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <csignal>
#include <functional>

#include "procutil.h"
#include "surface.h"

static QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

/* ------------------------------------------------------------ ClipboardState */

namespace {

/* wl-copy reads the payload from stdin, then forks a server to hold the
 * selection, so it outlives this QProcess by design. */
void wlCopy(const QByteArray &data, const QString &type)
{
    auto *proc = new QProcess(qApp);
    proc->setProgram(QStringLiteral("wl-copy"));
    if (!type.isEmpty())
        proc->setArguments({QStringLiteral("--type"), type});
    proc->setChildProcessModifier([]() {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
    });
    QObject::connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    QObject::connect(proc, &QProcess::errorOccurred, proc, &QObject::deleteLater);
    proc->start();
    proc->write(data);
    proc->closeWriteChannel();
}

} /* namespace */

void Clipboard::copyText(const QString &text)
{
    if (Surface::wayland())
        wlCopy(text.toUtf8(), QString());
    else
        QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

void Clipboard::copyImage(const QImage &image)
{
    if (!Surface::wayland()) {
        QGuiApplication::clipboard()->setImage(image);
        return;
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    wlCopy(png, QStringLiteral("image/png"));
}

ClipboardState *ClipboardState::instance()
{
    static ClipboardState *s = new ClipboardState();
    return s;
}

ClipboardState::ClipboardState(QObject *parent) : QObject(parent)
{
    if (!Surface::wayland()) {
        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
                &ClipboardState::onClipboardChanged);
        return;
    }
    available = !QStandardPaths::findExecutable(QStringLiteral("wl-paste")).isEmpty();
    if (!available)
        return;
    /* `--type text` lets wl-paste pick whichever text flavour the source
     * offers. --watch runs the command once per new selection with the
     * content on its stdin; the NUL marks the end, as wl-paste puts no
     * delimiter between them. Secrets a password manager tags with
     * x-kde-passwordManagerHint are drained and dropped. */
    m_watch = new BlockWatchProcess(
        {QStringLiteral("wl-paste"), QStringLiteral("--type"),
         QStringLiteral("text"), QStringLiteral("--watch"),
         QStringLiteral("sh"), QStringLiteral("-c"),
         QStringLiteral("if wl-paste --list-types 2>/dev/null"
                        " | grep -qx x-kde-passwordManagerHint; then"
                        " cat >/dev/null; else cat; printf '\\000'; fi")},
        QByteArray(1, '\0'), this);
    connect(m_watch, &BlockWatchProcess::block, this,
            [this](const QString &text) {
                if (!text.isEmpty())
                    addEntry(text);
            });
    m_watch->start();
}

void ClipboardState::onClipboardChanged()
{
    const QMimeData *mime =
        QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
    if (!mime || !mime->hasText())
        return;
    /* Password managers (KeePassXC, KDE's, ...) tag secrets with this type,
     * so a copied password never lands in the history popup. */
    if (mime->hasFormat(QStringLiteral("x-kde-passwordManagerHint")))
        return;
    const QString text = mime->text();
    if (!text.isEmpty())
        addEntry(text);
}

void ClipboardState::addEntry(const QString &text)
{
    /* Re-selecting a historical entry sets the clipboard, and dataChanged
     * hands it straight back to us -- dedup by removing any prior occurrence
     * first, so that just promotes it to the front instead of duplicating
     * it. Copying the same text twice from the source app lands here too. */
    history.removeAll(text);
    history.prepend(text);
    while (history.size() > maxHistory)
        history.removeLast();
    emit changed();
}

void ClipboardState::selectEntry(int index)
{
    if (index < 0 || index >= history.size())
        return;
    Clipboard::copyText(history[index]);
}

void ClipboardState::removeEntry(int index)
{
    if (index < 0 || index >= history.size())
        return;
    history.removeAt(index);
    emit changed();
}

void ClipboardState::clear()
{
    if (history.isEmpty())
        return;
    history.clear();
    emit changed();
}

/* ----------------------------------------------------------- ClipboardWidget */

ClipboardWidget::ClipboardWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(glyph(U'\U000F0147')); /* mdi clipboard */
    setTint(Theme::text);

    setVisible(ClipboardState::instance()->available);

    m_popup = new ClipboardPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);
    connect(this, &BarPill::rightClicked, this,
            []() { ClipboardState::instance()->clear(); });
}

/* -------------------------------------------------------------- history row */

namespace {

class ClipRow : public QWidget {
public:
    /* 34px, matching DeviceRow/LinkRow/WifiRow's row height elsewhere in the
     * popups. The remove affordance is a plain glyph in a fixed-width hit
     * zone, not a bordered ShellButton -- a full pill-shaped button sized
     * for a word ("Connect"/"Disconnect") looked oversized around a single
     * "×", nearly filling the row top-to-bottom. */
    static constexpr int kRowHeight = 34;
    static constexpr int kRemoveZoneWidth = 26;

    ClipRow(const QString &text, int index, QWidget *parent)
        : QWidget(parent), m_index(index)
    {
        /* Flatten to a single-line preview, same convention as the panel's
         * own window-title handling. */
        QString flat = text;
        for (QChar &c : flat)
            if (c == QLatin1Char('\n') || c == QLatin1Char('\t'))
                c = QLatin1Char(' ');
        m_preview = flat.trimmed();

        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(kRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    /* No Q_OBJECT on this row, so plain callbacks stand in for signals
     * (same idiom as WifiRow::onToggleExpand in network.cpp). */
    std::function<void(int)> selected;
    std::function<void(int)> removed;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(), m_hover ? Theme::surfaceHover
                                            : Theme::transparent,
                         Theme::smallRadius);

        const int textX = 8;
        const int removeX = width() - kRemoveZoneWidth;
        const int textW = removeX - 4 - textX;
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(Theme::text);
        const QFontMetrics fm(p.font());
        p.drawText(QRect(textX, 0, textW, height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_preview, Qt::ElideRight, textW));

        p.setFont(Theme::font(Theme::bodyFontSize));
        p.setPen(m_removeHover ? Theme::brightred : Theme::danger);
        p.drawText(QRect(removeX, 0, kRemoveZoneWidth, height()),
                   Qt::AlignCenter, QStringLiteral("×"));
    }

    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        m_removeHover = false;
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const bool overRemove = e->position().x() >= width() - kRemoveZoneWidth;
        if (overRemove != m_removeHover) {
            m_removeHover = overRemove;
            update();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            m_pressed = true;
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_pressed && e->button() == Qt::LeftButton
                && rect().contains(e->pos())) {
            if (e->position().x() >= width() - kRemoveZoneWidth) {
                if (removed)
                    removed(m_index);
            } else if (selected) {
                selected(m_index);
            }
        }
        m_pressed = false;
    }

private:
    QString m_preview;
    int m_index;
    bool m_hover = false;
    bool m_removeHover = false;
    bool m_pressed = false;
};

} /* namespace */

/* ------------------------------------------------------------ ClipboardPopup */

ClipboardPopup::ClipboardPopup(QWidget *anchor) : ContentPopup(anchor, 360, 480)
{
    /* Now only ever anchored from inside the System dropdown (ClipboardWidget
     * moved off the bar); as a submenu it shouldn't close its parent the way
     * a real top-level popup would (PopupManager::opened() only tears down
     * the previous popup tree, not siblings under the same one). */
    m_manageAsTopLevel = false;

    auto *lay = bodyLayout();

    auto *header = new QWidget(body());
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(Theme::rowSpacing);
    auto *title = new TextItem(header);
    title->setPixelSize(Theme::bodyFontSize);
    title->setBold(true);
    title->setColor(Theme::textStrong);
    title->setText(QStringLiteral("Clipboard"));
    hl->addWidget(title, 0, Qt::AlignVCenter);
    hl->addStretch(1);
    m_clearBtn = new ShellButton(QStringLiteral("Clear"), header);
    hl->addWidget(m_clearBtn, 0, Qt::AlignVCenter);
    lay->addWidget(header);

    connect(m_clearBtn, &ShellButton::activated, this,
            []() { ClipboardState::instance()->clear(); });

    lay->addWidget(new HLine(body()));

    m_listBox = new QWidget(body());
    auto *ll = new QVBoxLayout(m_listBox);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(Theme::popupSpacing);
    lay->addWidget(m_listBox);
    lay->addStretch(0);

    connect(ClipboardState::instance(), &ClipboardState::changed, this,
            &ClipboardPopup::rebuild);
    rebuild();
}

void ClipboardPopup::rebuild()
{
    ClipboardState *cs = ClipboardState::instance();

    auto *ll = static_cast<QVBoxLayout *>(m_listBox->layout());
    while (QLayoutItem *it = ll->takeAt(0)) {
        if (it->widget()) {
            it->widget()->hide();
            it->widget()->deleteLater();
        }
        delete it;
    }

    m_clearBtn->setEnabled(!cs->history.isEmpty());

    if (cs->history.isEmpty()) {
        auto *empty = new TextItem(m_listBox);
        empty->setPixelSize(Theme::smallFontSize);
        empty->setColor(Theme::textMuted);
        empty->setText(QStringLiteral("No clipboard history yet."));
        ll->addWidget(empty);
        empty->show();
    } else {
        for (int i = 0; i < cs->history.size(); i++) {
            auto *row = new ClipRow(cs->history[i], i, m_listBox);
            row->selected = [this](int idx) {
                ClipboardState::instance()->selectEntry(idx);
                closePopup();
            };
            row->removed = [](int idx) {
                ClipboardState::instance()->removeEntry(idx);
            };
            ll->addWidget(row);
            row->show();
        }
    }

    if (isVisible())
        relayout();
}
