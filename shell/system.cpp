#include "system.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

#include "bluetooth.h"
#include "clipboard.h"
#include "controlpanel.h"
#include "pills.h"
#include "powerprofiles.h"
#include "procutil.h"
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
    sr->addWidget(m_cpuBox, 1);
    sr->addWidget(m_memBox, 1);
    sr->addWidget(m_tempBox, 1);
    lay->addWidget(statsRow);

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

    /* Full-width, standard D-Bus power-profile selector. It hides itself on
     * machines without a net.hadess.PowerProfiles provider. */
    lay->addWidget(new PowerProfileWidget(body()));

    lay->addWidget(new HLine(body()));

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
