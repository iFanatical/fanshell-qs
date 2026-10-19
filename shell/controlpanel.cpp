#include "controlpanel.h"

#include <QFileInfo>
#include <QGridLayout>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

#include "displays.h"
#include "keyboard.h"
#include "mouse.h"
#include "overlay.h"
#include "paths.h"
#include "wallpaper.h"
#include "widgets.h"

namespace {

QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

QString minutes(int seconds)
{
    if (seconds % 3600 == 0)
        return QStringLiteral("%1 h").arg(seconds / 3600);
    if (seconds % 60 == 0)
        return QStringLiteral("%1 min").arg(seconds / 60);
    return QStringLiteral("%1 s").arg(seconds);
}

/* One square-ish button: an icon, a name, and a line of status. */
class Tile : public QWidget {
public:
    Tile(const QString &icon, const QString &title, QWidget *parent)
        : QWidget(parent), m_icon(icon), m_title(title)
    {
        setFixedHeight(104);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setStatus(const QString &status)
    {
        m_status = status;
        update();
    }

    std::function<void()> activated;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        Theme::paintRect(p, rect(), m_hover ? Theme::surfaceHover : Theme::surface,
                         Theme::radius, m_hover ? Theme::accent : Theme::border, 1);

        const QRect inner = rect().adjusted(10, 12, -10, -10);
        p.setFont(Theme::font(28));
        p.setPen(Theme::accent);
        p.drawText(QRect(inner.x(), inner.y(), inner.width(), 36),
                   Qt::AlignHCenter | Qt::AlignVCenter, m_icon);

        p.setFont(Theme::font(Theme::bodyFontSize, true));
        p.setPen(Theme::textStrong);
        p.drawText(QRect(inner.x(), inner.y() + 42, inner.width(), 20),
                   Qt::AlignHCenter | Qt::AlignVCenter, m_title);

        const QFont small = Theme::font(Theme::tinyFontSize);
        p.setFont(small);
        p.setPen(Theme::textMuted);
        const QString status = QFontMetrics(small).elidedText(
            m_status, Qt::ElideRight, inner.width());
        p.drawText(QRect(inner.x(), inner.y() + 64, inner.width(), 16),
                   Qt::AlignHCenter | Qt::AlignVCenter, status);
    }
    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())
                && activated)
            activated();
    }

private:
    QString m_icon;
    QString m_title;
    QString m_status;
    bool m_hover = false;
};

} /* namespace */

class ControlPanelWindow : public CenteredOverlay {
public:
    ControlPanelWindow()
        : CenteredOverlay(QStringLiteral("fanshell-control-panel"),
                          QStringLiteral("fanshell-qs-control-panel"), 540)
    {
        auto *lay = new QVBoxLayout(box());
        lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                                Theme::popupMargin, Theme::popupMargin);
        lay->setSpacing(Theme::popupSpacing);

        auto *title = new TextItem(box());
        title->setPixelSize(Theme::bodyFontSize);
        title->setBold(true);
        title->setColor(Theme::textStrong);
        title->setText(QStringLiteral("Control Panel"));
        lay->addWidget(title);

        auto *grid = new QGridLayout();
        grid->setSpacing(Theme::rowSpacing + 4);
        for (int c = 0; c < columns; c++)
            grid->setColumnStretch(c, 1);
        lay->addLayout(grid);

        /* Each tile hands over to its own window, which takes the panel's
         * place on screen. */
        auto add = [this, grid](char32_t icon, const QString &name,
                                std::function<void()> open) {
            auto *tile = new Tile(glyph(icon), name, box());
            tile->activated = [this, open]() {
                dismiss();
                open();
            };
            const int i = int(m_tiles.size());
            grid->addWidget(tile, i / columns, i % columns);
            m_tiles << tile;
            return tile;
        };
        m_displays = add(U'\U000F0A07', QStringLiteral("Displays"), []() {
            DisplaySettings::instance()->openWindow();
        });
        m_power = add(U'\U000F0D90', QStringLiteral("Screen power"), []() {
            ScreenPower::instance()->openWindow();
        });
        m_lock = add(U'\U000F097F', QStringLiteral("Screen lock"), []() {
            ScreenLock::instance()->openWindow();
        });
        m_keyboard = add(U'\U000F030C', QStringLiteral("Keyboard"), []() {
            KeyboardSettings::instance()->openWindow();
        });
        m_mouse = add(U'\U000F037D', QStringLiteral("Mouse & touchpad"), []() {
            MouseSettings::instance()->openWindow();
        });
        m_wallpaper = add(U'\U000F0E09', QStringLiteral("Wallpaper"), []() {
            WallpaperPicker::instance()->openPicker();
        });
    }

    void open()
    {
        sync();
        present();
    }

private:
    static constexpr int columns = 3;

    void sync()
    {
        const QVector<Displays::Output> outputs = Displays::query();
        QStringList on;
        for (const Displays::Output &o : outputs)
            if (o.enabled)
                on << o.name;
        if (on.size() == 1) {
            for (const Displays::Output &o : outputs)
                if (o.enabled)
                    m_displays->setStatus(QStringLiteral("%1 · %2×%3")
                                              .arg(o.name)
                                              .arg(o.width())
                                              .arg(o.height()));
        } else {
            m_displays->setStatus(QStringLiteral("%1 outputs").arg(on.size()));
        }

        const ScreenPower::Settings power = ScreenPower::instance()->live();
        m_power->setStatus(
            QStringLiteral("Blank %1 · DPMS %2")
                .arg(power.blank > 0 ? QStringLiteral("after ") + minutes(power.blank)
                                     : QStringLiteral("never"),
                     power.dpms ? QStringLiteral("on") : QStringLiteral("off")));

        const ScreenLock::Settings lock = ScreenLock::instance()->settings();
        m_lock->setStatus(lock.after > 0
                              ? QStringLiteral("Locks after ") + minutes(lock.after)
                              : (lock.beforeSuspend
                                     ? QStringLiteral("Only before sleep")
                                     : QStringLiteral("Off")));

        const KeyboardSettings::Settings keys = KeyboardSettings::instance()->settings();
        m_keyboard->setStatus(QStringLiteral("Repeat %1 ms, %2/s")
                                  .arg(keys.repeatDelay)
                                  .arg(keys.repeatRate));

        const QVector<MouseSettings::Device> pointers =
            MouseSettings::instance()->devices();
        m_mouse->setStatus(pointers.isEmpty() ? QStringLiteral("No devices")
                           : pointers.size() == 1
                               ? pointers.first().name
                               : QStringLiteral("%1 devices").arg(pointers.size()));

        const QString path = Paths::readConf(QStringLiteral("wallpaper.conf"))
                                 .value(QStringLiteral("path"));
        m_wallpaper->setStatus(path.isEmpty() ? QStringLiteral("None set")
                                              : QFileInfo(path).completeBaseName());
    }

    QVector<Tile *> m_tiles;
    Tile *m_displays;
    Tile *m_power;
    Tile *m_lock;
    Tile *m_keyboard;
    Tile *m_mouse;
    Tile *m_wallpaper;
};

ControlPanel *ControlPanel::instance()
{
    static ControlPanel *s = new ControlPanel();
    return s;
}

void ControlPanel::openWindow()
{
    if (!m_win)
        m_win = new ControlPanelWindow();
    m_win->open();
}

void ControlPanel::toggleWindow()
{
    if (m_win && m_win->isVisible())
        hideWindow();
    else
        openWindow();
}

void ControlPanel::hideWindow()
{
    if (m_win)
        m_win->dismiss();
}
