#include "system.h"

#include <QEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

#include "bluetooth.h"
#include "clipboard.h"
#include "controlpanel.h"
#include "desktopentry.h"
#include "gamemode.h"
#include "hardwarecontrols.h"
#include "idleinhibit.h"
#include "pills.h"
#include "powerprofiles.h"
#include "procutil.h"
#include "status.h"
#include "volume.h"

static QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

static void power(const QStringList &args)
{
    QProcess::startDetached(QStringLiteral("fanshell-qs-power"), args);
}

/* Declared in system.h (as `class StatBox *m_cpuBox` etc.), so it must live
 * at namespace scope, not anonymous -- unlike the other file-local widgets
 * below, which are never named outside this .cpp. A pill-styled tile (same
 * rounded surface-filled rect as BarPill) for one CPU/Mem/Temp stat; three
 * sit side by side instead of the stacked text rows this used to be. */
class StatBox : public QWidget {
public:
    static constexpr int kIconPx = 28;

    StatBox(const QString &icon, const QString &caption, QWidget *parent)
        : QWidget(parent), m_icon(icon), m_caption(caption)
    {
        setFixedHeight(76);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setValue(const QString &v)
    {
        if (m_value == v)
            return;
        m_value = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(), Theme::surface, Theme::radius);

        p.setFont(Theme::font(kIconPx));
        p.setPen(Theme::textStrong);
        p.drawText(QRectF(4, 6, width() - 8, 32), Qt::AlignCenter, m_icon);

        p.setFont(Theme::font(Theme::bodyFontSize, true));
        p.drawText(QRectF(4, 40, width() - 8, 20), Qt::AlignCenter, m_value);

        p.setFont(Theme::font(Theme::tinyFontSize, true));
        p.setPen(Theme::textMuted);
        p.drawText(QRectF(4, 60, width() - 8, 16), Qt::AlignCenter,
                  m_caption);
    }

private:
    QString m_icon, m_caption, m_value;
};

namespace {

TextItem *sectionLabel(const QString &text, QWidget *parent)
{
    auto *t = new TextItem(parent);
    t->setPixelSize(Theme::smallFontSize);
    t->setBold(true);
    t->setColor(Theme::textMuted);
    t->setText(text);
    return t;
}

/* Weather: icon on the left, temperature stacked above the condition text on
 * the right. A dedicated pill-styled tile rather than a generic ScriptPill,
 * since it needs the temp/condition on separate lines. */
class WeatherTile : public QWidget {
public:
    explicit WeatherTile(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(Theme::pillHeight * 2);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        hide(); /* hidden until the script reports something */

        m_proc.setCommand({"fanshell-qs-weather"});
        connect(&m_proc, &CollectorProcess::finished, this,
                &WeatherTile::parse);
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, &m_proc,
                [this]() { m_proc.start(); });
    }

    void setPolling(bool enabled)
    {
        if (enabled) {
            m_proc.start();
            m_timer->start(5 * 60 * 1000);
        } else {
            m_timer->stop();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(), Theme::surface, Theme::radius * 2);

        const int iconW = 36;
        p.setFont(Theme::font(28));
        p.setPen(Theme::text);
        p.drawText(QRectF(12, 0, iconW, height()),
                  Qt::AlignLeft | Qt::AlignVCenter, m_icon);

        const int textX = 12 + iconW + 8;
        const int textW = width() - textX - 12;
        p.setFont(Theme::font(Theme::bodyFontSize, true));
        p.drawText(QRectF(textX, 4, textW, 22),
                  Qt::AlignLeft | Qt::AlignVCenter, m_temp);

        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(Theme::textMuted);
        p.drawText(QRectF(textX, height() - 24, textW, 20),
                  Qt::AlignLeft | Qt::AlignVCenter, m_cond);
    }

private:
    void parse(const QString &text)
    {
        const QString trimmed = text.trimmed();
        setVisible(!trimmed.isEmpty());
        if (trimmed.isEmpty())
            return;
        const QStringList lines = trimmed.split('\n');
        for (const QString &line : lines) {
            const int i = line.indexOf('=');
            if (i < 0)
                continue;
            const QString k = line.left(i), v = line.mid(i + 1);
            if (k == QLatin1String("icon"))
                m_icon = v;
            else if (k == QLatin1String("temp"))
                m_temp = v;
            else if (k == QLatin1String("cond"))
                m_cond = v;
        }
        update();
    }

    CollectorProcess m_proc;
    QTimer *m_timer;
    QString m_icon, m_temp, m_cond;
};

/* Output-device row, identical in spirit to network.cpp's WifiRow/LinkRow:
 * click to make this sink the default. */
class SinkRow : public QWidget {
public:
    SinkRow(const PulseBackend::Sink &s, QWidget *parent)
        : QWidget(parent), m_s(s)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(24);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(),
                         m_hover ? Theme::surfaceHover
                                 : (m_s.isDefault ? Theme::surface
                                                  : Theme::transparent),
                         Theme::smallRadius);
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(m_s.isDefault ? Theme::accent : Theme::text);
        const QString label = (m_s.isDefault ? QStringLiteral(" ")
                                             : QString())
                              + m_s.description;
        const QFontMetrics fm(p.font());
        p.drawText(QRect(8, 0, width() - 16, height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(label, Qt::ElideRight, width() - 16));
    }

    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            m_pressed = true;
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_pressed && e->button() == Qt::LeftButton
                && rect().contains(e->pos()))
            PulseBackend::instance()->setDefaultSink(m_s.name);
        m_pressed = false;
    }

private:
    PulseBackend::Sink m_s;
    bool m_hover = false;
    bool m_pressed = false;
};

/* A program's icon: the one its stream names, else that of the desktop
 * entry whose id or command is the program, else a generic one. */
QIcon programIcon(const PulseBackend::Stream &s)
{
    if (!s.iconName.isEmpty() && QIcon::hasThemeIcon(s.iconName))
        return QIcon::fromTheme(s.iconName);
    const QStringList wanted{s.binary.toLower(), s.name.toLower()};
    for (const DesktopEntry &e : DesktopEntries::instance()->applications()) {
        if (e.icon.isEmpty())
            continue;
        const QString id = e.id.toLower();
        const QString command =
            e.exec.section(QLatin1Char(' '), 0, 0).section(QLatin1Char('/'), -1).toLower();
        for (const QString &w : wanted)
            if (!w.isEmpty()
                    && (id == w || id.endsWith(QLatin1Char('.') + w) || command == w
                        || e.name.toLower() == w))
                return QFileInfo::exists(e.icon) ? QIcon(e.icon)
                                                 : QIcon::fromTheme(e.icon);
    }
    return QIcon::fromTheme(QStringLiteral("application-x-executable"));
}

/* The icon column of an app's row: same width as the Output and Input
 * icons, so the sliders line up. Click to mute; muted, it is greyed. */
class StreamIcon : public QWidget {
public:
    explicit StreamIcon(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(20, 20);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> clicked;

    void set(const QIcon &icon, const QString &name, bool muted)
    {
        m_icon = icon;
        m_name = name;
        m_muted = muted;
        setToolTip(QStringLiteral("%1. Click to %2")
                       .arg(name, muted ? QStringLiteral("unmute") : QStringLiteral("mute")));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect r = rect().adjusted(1, 1, -1, -1);
        if (!m_icon.isNull()) {
            m_icon.paint(&p, r, Qt::AlignCenter, m_muted ? QIcon::Disabled : QIcon::Normal);
        } else {
            p.setFont(Theme::font(Theme::smallFontSize, true));
            p.setPen(m_muted ? Theme::textMuted : Theme::text);
            p.drawText(rect(), Qt::AlignCenter, m_name.left(1).toUpper());
        }
        if (m_muted) {
            p.setPen(QPen(Theme::danger, 2));
            p.drawLine(r.bottomLeft(), r.topRight());
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && clicked)
            clicked();
    }

private:
    QIcon m_icon;
    QString m_name;
    bool m_muted = false;
};

/* One program's volume: its icon (click to mute), a slider and the level. */
class StreamRow : public QWidget {
public:
    StreamRow(const PulseBackend::Stream &s, QWidget *parent)
        : QWidget(parent), m_index(s.index), m_iconImage(programIcon(s))
    {
        auto *l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(Theme::rowSpacing);
        m_icon = new StreamIcon(this);
        m_slider = new ValueSlider(this);
        m_slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_pct = new TextItem(this);
        m_pct->setPixelSize(Theme::smallFontSize);
        m_pct->setHAlign(Qt::AlignRight);
        m_pct->setFixedWidth(44);
        l->addWidget(m_icon, 0, Qt::AlignVCenter);
        l->addWidget(m_slider, 1, Qt::AlignVCenter);
        l->addWidget(m_pct, 0, Qt::AlignVCenter);
        PulseBackend *pa = PulseBackend::instance();
        const uint32_t index = m_index;
        m_icon->clicked = [this, pa, index]() { pa->setStreamMuted(index, !m_muted); };
        QObject::connect(m_slider, &ValueSlider::moved, this,
                         [pa, index](qreal v) { pa->setStreamVolume(index, v); });
        sync(s);
    }

    void sync(const PulseBackend::Stream &s)
    {
        m_muted = s.muted;
        m_icon->set(m_iconImage, s.name, s.muted);
        m_slider->setValue(qMin<qreal>(1, s.volume));
        /* Paused streams are dimmed like muted ones, but keep their level. */
        m_slider->setFillColor(s.muted || !s.playing ? Theme::textMuted : Theme::accent);
        m_pct->setText(s.muted ? QStringLiteral("Muted")
                               : QString::number(qRound(s.volume * 100))
                                     + QStringLiteral("%"));
    }

private:
    uint32_t m_index;
    bool m_muted = false;
    QIcon m_iconImage;
    StreamIcon *m_icon;
    ValueSlider *m_slider;
    TextItem *m_pct;
};

/* One icon in the power row. While the pointer is over it, its name shows in
 * the caption below the row. A `confirm` action needs a second click within
 * four seconds, and the caption says so meanwhile. No Q_OBJECT: plain
 * callbacks, like ClipRow in clipboard.cpp. */
class PowerButton : public ShellButton {
public:
    PowerButton(const QString &icon, const QString &name, bool confirm,
                QWidget *parent)
        : ShellButton(QString(), parent), m_icon(icon), m_name(name),
          m_confirm(confirm)
    {
        setFixedHeight(Theme::buttonHeight + 8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        m_revert = new QTimer(this);
        m_revert->setSingleShot(true);
        connect(m_revert, &QTimer::timeout, this, [this]() { disarm(); });
        connect(this, &ShellButton::activated, this, [this]() {
            if (!m_confirm || m_armed) {
                disarm();
                if (triggered)
                    triggered();
                return;
            }
            m_armed = true;
            m_revert->start(4000);
            showCaption();
            update();
        });
    }

    std::function<void()> triggered;
    /* Sets the row's caption; an empty string clears it. */
    std::function<void(const QString &)> caption;

    QSize sizeHint() const override { return QSize(44, height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor edge = m_armed ? Theme::danger
                                    : (m_hover ? Theme::accent : Theme::border);
        Theme::paintRect(p, rect(), m_hover ? Theme::surfaceHover : Theme::surface,
                         Theme::radius, edge, 1);
        p.setFont(Theme::font(18));
        p.setPen(m_armed ? Theme::danger : (m_hover ? Theme::accent : Theme::text));
        p.drawText(rect(), Qt::AlignCenter, m_icon);
    }
    void enterEvent(QEnterEvent *e) override
    {
        ShellButton::enterEvent(e);
        m_hover = true;
        showCaption();
    }
    void leaveEvent(QEvent *e) override
    {
        ShellButton::leaveEvent(e);
        m_hover = false;
        if (caption && !m_armed)
            caption(QString());
    }

private:
    void showCaption()
    {
        if (caption)
            caption(m_armed ? QStringLiteral("Click again to %1")
                                  .arg(m_name.toLower())
                            : m_name);
    }
    void disarm()
    {
        const bool was = m_armed;
        m_armed = false;
        m_revert->stop();
        update();
        if (was && caption)
            caption(m_hover ? m_name : QString());
    }

    QString m_icon;
    QString m_name;
    bool m_confirm;
    bool m_armed = false;
    bool m_hover = false;
    QTimer *m_revert;
};

} /* namespace */

/* ----------------------------------------------------------- SystemWidget */

SystemWidget::SystemWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(glyph(U'\U000F0493')); /* mdi cog */
    setTint(Theme::text);

    m_popup = new SystemPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);
}

/* ------------------------------------------------------------ SystemPopup */

SystemPopup::SystemPopup(QWidget *anchor) : ContentPopup(anchor, 360)
{
    auto *lay = bodyLayout();
    PulseBackend *pa = PulseBackend::instance();

    /* Settings live in the Control Panel; this dropdown is status and
     * quick actions. */
    auto *header = new QWidget(body());
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(Theme::rowSpacing);
    auto *title = new TextItem(header);
    title->setPixelSize(Theme::bodyFontSize);
    title->setBold(true);
    title->setColor(Theme::textStrong);
    title->setText(QStringLiteral("System"));
    auto *panelBtn = new ShellButton(
        glyph(U'\U000F062E') + QStringLiteral("  Control Panel"), header);
    hl->addWidget(title, 1, Qt::AlignLeft | Qt::AlignVCenter);
    hl->addWidget(panelBtn);
    lay->addWidget(header);
    connect(panelBtn, &ShellButton::activated, this, [this]() {
        closePopup();
        ControlPanel::instance()->openWindow();
    });

    lay->addWidget(new HLine(body()));

    /* ---- CPU / memory / temperature: three tiles side by side ---- */
    auto *statsRow = new QWidget(body());
    auto *sr = new QHBoxLayout(statsRow);
    sr->setContentsMargins(0, 0, 0, 0);
    sr->setSpacing(Theme::rowSpacing);
    m_cpuBox = new StatBox(glyph(U'\U000F0EE0'), QStringLiteral("CPU"),
                           statsRow);
    m_memBox = new StatBox(glyph(U'\U000F035B'), QStringLiteral("Memory"),
                           statsRow);
    m_tempBox = new StatBox(glyph(U'\U000F050F'), QStringLiteral("Temp"),
                            statsRow);
    m_diskBox = new StatBox(glyph(U'\U000F02CA'), QStringLiteral("Disk"),
                            statsRow);
    sr->addWidget(m_cpuBox, 1);
    sr->addWidget(m_memBox, 1);
    sr->addWidget(m_tempBox, 1);
    sr->addWidget(m_diskBox, 1);
    lay->addWidget(statsRow);
    connect(DiskState::instance(), &DiskState::changed, this, &SystemPopup::syncDisk);
    syncDisk();

    auto *sysProc = new CollectorProcess(this);
    sysProc->setCommand({"fanshell-qs-sysinfo"});
    connect(sysProc, &CollectorProcess::finished, this,
            &SystemPopup::parseSysInfo);
    auto *sysTimer = new QTimer(this);
    connect(sysTimer, &QTimer::timeout, sysProc,
            [sysProc]() { sysProc->start(); });
    connect(this, &ShellPopup::popupVisibleChanged, this,
            [sysTimer, sysProc](bool visible) {
                if (visible) {
                    sysProc->start();
                    sysTimer->start(3000);
                    DiskState::instance()->refresh();
                } else {
                    sysTimer->stop();
                }
                BluetoothState::instance()->setPolling(visible);
            });

    lay->addWidget(new HLine(body()));

    /* Weather takes the flexible part of the quick row; Bluetooth and
     * Clipboard retain their compact fixed widths. VPN lives in its own
     * full-width selector tile immediately below. */
    auto *quickRow = new QWidget(body());
    auto *qr = new QHBoxLayout(quickRow);
    qr->setContentsMargins(0, 0, 0, 0);
    qr->setSpacing(Theme::rowSpacing);
    auto *weather = new WeatherTile(quickRow);
    auto *bt = new BluetoothWidget(quickRow);
    auto *clip = new ClipboardWidget(quickRow);
    bt->setLarge(true);
    clip->setLarge(true);
    qr->addWidget(weather, 1, Qt::AlignVCenter);
    qr->addWidget(bt, 0, Qt::AlignVCenter);
    qr->addWidget(clip, 0, Qt::AlignVCenter);
    lay->addWidget(quickRow);
    connect(this, &ShellPopup::popupVisibleChanged, weather,
            [weather](bool visible) { weather->setPolling(visible); });

    lay->addWidget(new VpnWidget(body()));

    /* ---- Keep awake / game mode ---- */
    auto *modeRow = new QWidget(body());
    auto *mr = new QHBoxLayout(modeRow);
    mr->setContentsMargins(0, 0, 0, 0);
    mr->setSpacing(Theme::rowSpacing);
    auto *awake = new ShellButton(
        glyph(U'\U000F0176') + QStringLiteral("  Keep awake"), modeRow); /* mdi coffee */
    awake->setToolTip(QStringLiteral("Do not blank or lock the screen until turned off"));
    auto *game = new ShellButton(
        glyph(U'\U000F0297') + QStringLiteral("  Game mode"), modeRow); /* mdi gamepad */
    game->setToolTip(QStringLiteral("Do not disturb, performance profile, night light "
                                    "held, no blanking or locking"));
    for (ShellButton *b : {awake, game})
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mr->addWidget(awake, 1);
    mr->addWidget(game, 1);
    lay->addWidget(modeRow);
    auto syncModes = [awake, game]() {
        awake->setActive(IdleInhibitor::instance()->keepAwake());
        game->setActive(GameMode::instance()->active());
    };
    connect(awake, &ShellButton::activated, this, []() {
        IdleInhibitor *i = IdleInhibitor::instance();
        i->setKeepAwake(!i->keepAwake());
    });
    connect(game, &ShellButton::activated, this, []() { GameMode::instance()->toggle(); });
    connect(IdleInhibitor::instance(), &IdleInhibitor::changed, this, syncModes);
    connect(GameMode::instance(), &GameMode::changed, this, syncModes);
    syncModes();

    /* Full-width, standard D-Bus power-profile selector. It hides itself on
     * machines without a net.hadess.PowerProfiles provider. */
    lay->addWidget(new PowerProfileWidget(body()));

    lay->addWidget(new HLine(body()));

    /* ---- Keyboard backlight, on a machine that has one ---- */
    HardwareControls *hw = HardwareControls::instance();
    if (hw->hasKeyboardBacklight() && hw->keyboardMax() > 0) {
        lay->addWidget(sectionLabel(QStringLiteral("Keyboard backlight"),
                                    body()));
        auto *kbdRow = new QWidget(body());
        auto *kl = new QHBoxLayout(kbdRow);
        kl->setContentsMargins(0, 0, 0, 0);
        kl->setSpacing(Theme::rowSpacing);
        m_kbdIcon = new ClickableText(kbdRow);
        m_kbdIcon->setPixelSize(Theme::bodyFontSize);
        m_kbdIcon->setFixedWidth(20);
        m_kbdSlider = new ValueSlider(kbdRow);
        m_kbdSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_kbdPct = new TextItem(kbdRow);
        m_kbdPct->setPixelSize(Theme::smallFontSize);
        m_kbdPct->setHAlign(Qt::AlignRight);
        m_kbdPct->setFixedWidth(44);
        kl->addWidget(m_kbdIcon, 0, Qt::AlignVCenter);
        kl->addWidget(m_kbdSlider, 1, Qt::AlignVCenter);
        kl->addWidget(m_kbdPct, 0, Qt::AlignVCenter);
        lay->addWidget(kbdRow);

        /* The icon switches it off, and back on to where it was. */
        connect(m_kbdIcon, &ClickableText::clicked, this, [this, hw]() {
            if (hw->keyboardLevel() > 0)
                hw->setKeyboardBacklight(0);
            else
                hw->setKeyboardBacklight(m_kbdLastOn > 0 ? m_kbdLastOn
                                                         : hw->keyboardMax());
        });
        /* Snaps to the device's levels: three on a ThinkPad. */
        connect(m_kbdSlider, &ValueSlider::moved, this, [hw](qreal v) {
            hw->setKeyboardBacklight(qRound(v * hw->keyboardMax()));
        });
        connect(hw, &HardwareControls::keyboardBacklightChanged, this,
                &SystemPopup::syncKeyboard);
        connect(this, &ShellPopup::popupVisibleChanged, this,
                [hw](bool visible) {
                    if (visible)
                        hw->refreshKeyboardBacklight();
                });
        syncKeyboard();
        lay->addWidget(new HLine(body()));
    }

    /* ---- Output ---- */
    lay->addWidget(sectionLabel(QStringLiteral("Output"), body()));
    auto *outRow = new QWidget(body());
    auto *ol = new QHBoxLayout(outRow);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->setSpacing(Theme::rowSpacing);
    m_outIcon = new ClickableText(outRow);
    m_outIcon->setPixelSize(Theme::bodyFontSize);
    m_outIcon->setFixedWidth(20);
    m_outSlider = new ValueSlider(outRow);
    m_outSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_outPct = new TextItem(outRow);
    m_outPct->setPixelSize(Theme::smallFontSize);
    m_outPct->setHAlign(Qt::AlignRight);
    m_outPct->setFixedWidth(44);
    ol->addWidget(m_outIcon, 0, Qt::AlignVCenter);
    ol->addWidget(m_outSlider, 1, Qt::AlignVCenter);
    ol->addWidget(m_outPct, 0, Qt::AlignVCenter);
    lay->addWidget(outRow);

    connect(m_outIcon, &ClickableText::clicked, this, [pa]() {
        if (pa->hasSink)
            pa->toggleSinkMuted();
    });
    connect(m_outSlider, &ValueSlider::moved, this, [pa](qreal v) {
        if (pa->hasSink)
            pa->setSinkVolume(v);
    });

    /* ---- Input (mic) ---- */
    lay->addWidget(sectionLabel(QStringLiteral("Input"), body()));
    auto *micRow = new QWidget(body());
    auto *ml = new QHBoxLayout(micRow);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(Theme::rowSpacing);
    m_micIcon = new ClickableText(micRow);
    m_micIcon->setPixelSize(Theme::bodyFontSize);
    m_micIcon->setFixedWidth(20);
    m_micSlider = new ValueSlider(micRow);
    m_micSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_micPct = new TextItem(micRow);
    m_micPct->setPixelSize(Theme::smallFontSize);
    m_micPct->setHAlign(Qt::AlignRight);
    m_micPct->setFixedWidth(44);
    ml->addWidget(m_micIcon, 0, Qt::AlignVCenter);
    ml->addWidget(m_micSlider, 1, Qt::AlignVCenter);
    ml->addWidget(m_micPct, 0, Qt::AlignVCenter);
    lay->addWidget(micRow);

    connect(m_micIcon, &ClickableText::clicked, this, [pa]() {
        if (pa->hasSource)
            pa->toggleSourceMuted();
    });
    connect(m_micSlider, &ValueSlider::moved, this, [pa](qreal v) {
        if (pa->hasSource)
            pa->setSourceVolume(v);
    });

    /* ---- Each program's volume, while any has a stream ---- */
    m_appsLabel = sectionLabel(QStringLiteral("Apps"), body());
    lay->addWidget(m_appsLabel);
    m_appsBox = new QWidget(body());
    auto *al = new QVBoxLayout(m_appsBox);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(Theme::rowSpacing);
    lay->addWidget(m_appsBox);
    connect(pa, &PulseBackend::streamsChanged, this, &SystemPopup::syncApps);

    lay->addWidget(new HLine(body()));
    lay->addWidget(sectionLabel(QStringLiteral("Output device"), body()));

    m_deviceBox = new QWidget(body());
    auto *dl = new QVBoxLayout(m_deviceBox);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->setSpacing(Theme::popupSpacing);
    lay->addWidget(m_deviceBox);

    connect(pa, &PulseBackend::changed, this, &SystemPopup::syncAudio);

    /* ---- Power: one row of icons, named in the caption below ---- */
    lay->addWidget(new HLine(body()));
    auto *powerRow = new QWidget(body());
    auto *pr = new QHBoxLayout(powerRow);
    pr->setContentsMargins(0, 0, 0, 0);
    pr->setSpacing(Theme::rowSpacing);
    auto *caption = new TextItem(body());
    caption->setPixelSize(Theme::smallFontSize);
    caption->setColor(Theme::textMuted);
    caption->setHAlign(Qt::AlignHCenter);
    caption->setFixedHeight(16);
    struct Action {
        char32_t icon;
        const char *name;
        const char *arg;
        bool confirm;
    };
    for (const Action &a : {Action{U'\U000F033E', "Lock", "lock", false},
                            Action{U'\U000F0904', "Sleep", "suspend", false},
                            Action{U'\U000F0343', "Log out", "logout", true},
                            Action{U'\U000F0709', "Reboot", "reboot", true},
                            Action{U'\U000F0425', "Power off", "poweroff", true}}) {
        auto *b = new PowerButton(glyph(a.icon), QString::fromUtf8(a.name),
                                  a.confirm, powerRow);
        b->caption = [caption](const QString &text) { caption->setText(text); };
        const QString arg = QString::fromUtf8(a.arg);
        b->triggered = [this, arg]() {
            power({arg});
            closePopup();
        };
        pr->addWidget(b, 1);
    }
    lay->addWidget(powerRow);
    lay->addWidget(caption, 0, Qt::AlignHCenter);

    lay->addStretch(0);

    syncAudio();
    syncApps();
}

void SystemPopup::syncApps()
{
    const QVector<PulseBackend::Stream> &streams = PulseBackend::instance()->streams;
    QVector<uint> key;
    for (const PulseBackend::Stream &s : streams)
        key << s.index;
    auto *al = static_cast<QVBoxLayout *>(m_appsBox->layout());
    if (key != m_appsKey) {
        m_appsKey = key;
        while (QLayoutItem *it = al->takeAt(0)) {
            if (it->widget()) {
                it->widget()->hide();
                it->widget()->deleteLater();
            }
            delete it;
        }
        for (const PulseBackend::Stream &s : streams) {
            auto *row = new StreamRow(s, m_appsBox);
            al->addWidget(row);
            row->show();
        }
        m_appsLabel->setVisible(!streams.isEmpty());
        m_appsBox->setVisible(!streams.isEmpty());
        if (isVisible())
            relayout();
        return;
    }
    for (int i = 0; i < streams.size(); i++)
        if (auto *row = static_cast<StreamRow *>(al->itemAt(i)->widget()))
            row->sync(streams[i]);
}

void SystemPopup::parseSysInfo(const QString &text)
{
    int cpu = -1, memPct = -1;
    QString tempStr;
    const QStringList lines = text.trimmed().split('\n');
    for (const QString &line : lines) {
        const int i = line.indexOf('=');
        if (i < 0)
            continue;
        const QString k = line.left(i), v = line.mid(i + 1);
        if (k == QLatin1String("cpu"))
            cpu = v.toInt();
        else if (k == QLatin1String("mem_pct"))
            memPct = v.toInt();
        else if (k == QLatin1String("temp"))
            tempStr = v;
    }

    if (cpu >= 0)
        m_cpuBox->setValue(QStringLiteral("%1%").arg(cpu));
    if (memPct >= 0)
        m_memBox->setValue(QStringLiteral("%1%").arg(memPct));
    m_tempBox->setVisible(!tempStr.isEmpty());
    if (!tempStr.isEmpty())
        m_tempBox->setValue(QStringLiteral("%1°C").arg(tempStr));

    if (isVisible())
        relayout();
}

void SystemPopup::syncDisk()
{
    const DiskState::Mount m = DiskState::instance()->fullest();
    m_diskBox->setVisible(m.percent >= 0);
    m_diskBox->setValue(QStringLiteral("%1%").arg(m.percent));
    QStringList tip;
    for (const DiskState::Mount &d : DiskState::instance()->mounts())
        tip << QStringLiteral("%1: %2% full, %3 free")
                   .arg(d.path)
                   .arg(d.percent)
                   .arg(NetSpeed::rate(double(d.freeBytes)));
    m_diskBox->setToolTip(tip.join(QLatin1Char('\n')));
    if (isVisible())
        relayout();
}

void SystemPopup::syncAudio()
{
    PulseBackend *pa = PulseBackend::instance();

    m_outIcon->setText(pa->sinkMuted ? glyph(U'\U000F075F') : glyph(U''));
    m_outIcon->setColor(pa->sinkMuted ? Theme::danger : Theme::text);
    m_outSlider->setValue(pa->sinkVolume);
    m_outSlider->setFillColor(pa->sinkMuted ? Theme::textMuted
                                            : Theme::accent);
    m_outPct->setText(QString::number(qRound(pa->sinkVolume * 100))
                      + QStringLiteral("%"));

    m_micIcon->setText(pa->sourceMuted ? glyph(U'') : glyph(U''));
    m_micIcon->setColor(pa->sourceMuted ? Theme::danger : Theme::text);
    m_micSlider->setValue(pa->sourceVolume);
    m_micSlider->setFillColor(pa->sourceMuted ? Theme::textMuted
                                              : Theme::success);
    m_micPct->setText(QString::number(qRound(pa->sourceVolume * 100))
                      + QStringLiteral("%"));

    rebuildDevices();
}

void SystemPopup::syncKeyboard()
{
    if (!m_kbdSlider)
        return;
    HardwareControls *hw = HardwareControls::instance();
    const int level = hw->keyboardLevel();
    const int max = qMax(1, hw->keyboardMax());
    if (level > 0)
        m_kbdLastOn = level;
    /* mdi keyboard / keyboard-off-outline */
    m_kbdIcon->setText(level > 0 ? glyph(U'\U000F030C')
                                 : glyph(U'\U000F0E4B'));
    m_kbdIcon->setColor(level > 0 ? Theme::text : Theme::textMuted);
    m_kbdSlider->setValue(qreal(level) / max);
    m_kbdSlider->setFillColor(level > 0 ? Theme::brightmagenta
                                        : Theme::textMuted);
    m_kbdPct->setText(level > 0 ? QString::number(qRound(level * 100.0 / max))
                                      + QStringLiteral("%")
                                : QStringLiteral("Off"));
}

void SystemPopup::rebuildDevices()
{
    PulseBackend *pa = PulseBackend::instance();
    QStringList key;
    for (const PulseBackend::Sink &s : pa->sinks)
        key << s.name + (s.isDefault ? QStringLiteral("*") : QString());
    if (key == m_deviceKey)
        return;
    m_deviceKey = key;

    auto *dl = static_cast<QVBoxLayout *>(m_deviceBox->layout());
    while (QLayoutItem *it = dl->takeAt(0)) {
        if (it->widget()) {
            it->widget()->hide(); /* deleteLater leaves it painted a tick */
            it->widget()->deleteLater();
        }
        delete it;
    }
    for (const PulseBackend::Sink &s : pa->sinks) {
        auto *row = new SinkRow(s, m_deviceBox);
        dl->addWidget(row);
        row->show();
    }
    if (isVisible())
        relayout();
}
