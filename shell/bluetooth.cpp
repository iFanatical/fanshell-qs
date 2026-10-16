#include "bluetooth.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>

static QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

/* ---------------------------------------------------------- BluetoothState */

BluetoothState *BluetoothState::instance()
{
    static BluetoothState *s = new BluetoothState();
    return s;
}

BluetoothState::BluetoothState(QObject *parent)
    : QObject(parent), m_watch({"fanshell-qs-bt", "watch", "3"})
{
    connect(&m_watch, &BlockWatchProcess::block, this,
            &BluetoothState::parse);
    connect(&m_action, &CollectorProcess::finished, this,
            &BluetoothState::parse);
}

void BluetoothState::setPolling(bool enabled)
{
    if (m_polling == enabled)
        return;
    m_polling = enabled;
    if (enabled)
        m_watch.start(); /* watcher emits an immediate status block */
    else
        m_watch.stop();
}

void BluetoothState::parse(const QString &block)
{
    const bool oldPowered = powered, oldDiscovering = discovering;
    const QStringList lines = block.trimmed().split('\n');
    QVector<Device> devs;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("device\t"))) {
            const QStringList p = line.split('\t'); /* device,mac,name,conn,paired */
            if (p.size() >= 3)
                devs.push_back({p[1], p[2],
                                p.value(3) == QLatin1String("1"),
                                p.value(4) == QLatin1String("1")});
        } else {
            const int i = line.indexOf('=');
            if (i < 0)
                continue;
            const QString k = line.left(i), v = line.mid(i + 1);
            if (k == QLatin1String("powered"))
                powered = (v == QLatin1String("yes"));
            else if (k == QLatin1String("discovering"))
                discovering = (v == QLatin1String("yes"));
        }
    }
    /* The watcher repeats its status every few seconds. Rebuilding the
     * popup's rows each time would recreate the button under the cursor and
     * drop a click that straddles the rebuild, so only report real changes. */
    auto sameDevices = [](const QVector<Device> &a, const QVector<Device> &b) {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i)
            if (a[i].mac != b[i].mac || a[i].name != b[i].name
                    || a[i].connected != b[i].connected
                    || a[i].paired != b[i].paired)
                return false;
        return true;
    };
    if (powered == oldPowered && discovering == oldDiscovering
            && sameDevices(devices, devs))
        return;
    devices = devs;
    emit changed();
}

void BluetoothState::action(const QStringList &args)
{
    m_action.start(QStringList{QStringLiteral("fanshell-qs-bt")} + args);
}

void BluetoothState::togglePower()
{
    action({QStringLiteral("power"),
            powered ? QStringLiteral("off") : QStringLiteral("on")});
}

void BluetoothState::scan() { action({QStringLiteral("scan"), QStringLiteral("8")}); }
void BluetoothState::connectDevice(const QString &mac) { action({QStringLiteral("connect"), mac}); }
void BluetoothState::disconnectDevice(const QString &mac) { action({QStringLiteral("disconnect"), mac}); }

/* --------------------------------------------------------- BluetoothWidget */

BluetoothWidget::BluetoothWidget(QWidget *parent) : BarPill(parent)
{
    m_popup = new BluetoothPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);
    connect(BluetoothState::instance(), &BluetoothState::changed, this,
            &BluetoothWidget::sync);
    sync();
}

void BluetoothWidget::sync()
{
    BluetoothState *bt = BluetoothState::instance();
    setIcon(!bt->powered
                ? glyph(U'\U000F00B2')
                : (bt->connectedCount() > 0 ? glyph(U'\U000F00B1')
                                            : glyph(U'\U000F00AF')));
    setTint(bt->connectedCount() > 0 ? Theme::accent : Theme::text);
}

/* -------------------------------------------------------------- device row */

namespace {

class DeviceRow : public QWidget {
public:
    DeviceRow(const BluetoothState::Device &d, QWidget *parent)
        : QWidget(parent), m_d(d)
    {
        setMouseTracking(true);
        setFixedHeight(34);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_btn = new ShellButton(d.connected ? QStringLiteral("Disconnect")
                                            : QStringLiteral("Connect"),
                                this);
        m_btn->setDanger(d.connected);
        QObject::connect(m_btn, &ShellButton::activated, this, [this]() {
            if (m_d.connected)
                BluetoothState::instance()->disconnectDevice(m_d.mac);
            else
                BluetoothState::instance()->connectDevice(m_d.mac);
        });
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        const QSize b = m_btn->sizeHint();
        m_btn->setGeometry(width() - 6 - b.width(), (34 - b.height()) / 2,
                           b.width(), b.height());
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintRect(p, rect(),
                         m_hover ? Theme::surfaceHover
                                 : (m_d.connected ? Theme::surface
                                                  : Theme::transparent),
                         Theme::smallRadius);
        const int textX = 8;
        const int textW = m_btn->x() - 8 - textX;
        const QFontMetrics fmS(Theme::font(Theme::smallFontSize));
        const QFontMetrics fmT(Theme::font(Theme::tinyFontSize));
        const int colH = fmS.height() + fmT.height();
        const int colY = int((34 - colH) / 2.0);
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(Theme::text);
        p.drawText(QRect(textX, colY, textW, fmS.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fmS.elidedText(m_d.name, Qt::ElideRight, textW));
        p.setFont(Theme::font(Theme::tinyFontSize));
        p.setPen(m_d.connected ? Theme::success : Theme::textMuted);
        const QString status = m_d.connected
                                   ? QStringLiteral("Connected")
                                   : (m_d.paired ? QStringLiteral("Paired")
                                                 : QStringLiteral("Available"));
        p.drawText(QRect(textX, colY + fmS.height(), textW, fmT.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   status + QStringLiteral(" · ") + m_d.mac);
    }

    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }

private:
    BluetoothState::Device m_d;
    bool m_hover = false;
    ShellButton *m_btn;
};

} /* namespace */

/* ---------------------------------------------------------- BluetoothPopup */

BluetoothPopup::BluetoothPopup(QWidget *anchor) : ContentPopup(anchor, 320)
{
    /* Now only ever anchored from inside the System dropdown (BluetoothWidget
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
    title->setText(QStringLiteral("Bluetooth"));
    hl->addWidget(title, 0, Qt::AlignVCenter);
    hl->addStretch(1);
    m_scanBtn = new ShellButton(QStringLiteral("Scan"), header);
    m_powerBtn = new ShellButton(QStringLiteral("Off"), header);
    hl->addWidget(m_scanBtn, 0, Qt::AlignVCenter);
    hl->addWidget(m_powerBtn, 0, Qt::AlignVCenter);
    lay->addWidget(header);

    connect(m_powerBtn, &ShellButton::activated, this,
            []() { BluetoothState::instance()->togglePower(); });
    connect(m_scanBtn, &ShellButton::activated, this,
            []() { BluetoothState::instance()->scan(); });

    lay->addWidget(new HLine(body()));

    m_listBox = new QWidget(body());
    auto *ll = new QVBoxLayout(m_listBox);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(Theme::popupSpacing);
    lay->addWidget(m_listBox);
    lay->addStretch(0);

    connect(BluetoothState::instance(), &BluetoothState::changed, this,
            &BluetoothPopup::rebuild);
    rebuild();
}

void BluetoothPopup::rebuild()
{
    BluetoothState *bt = BluetoothState::instance();

    m_powerBtn->setLabel(bt->powered ? QStringLiteral("On")
                                     : QStringLiteral("Off"));
    m_powerBtn->setActive(bt->powered);
    m_scanBtn->setLabel(bt->discovering ? QStringLiteral("Scanning…")
                                        : QStringLiteral("Scan"));
    m_scanBtn->setEnabled(bt->powered);

    auto *ll = static_cast<QVBoxLayout *>(m_listBox->layout());
    while (QLayoutItem *it = ll->takeAt(0)) {
        if (it->widget()) {
            it->widget()->hide(); /* deleteLater leaves it painted a tick */
            it->widget()->deleteLater();
        }
        delete it;
    }

    if (bt->devices.isEmpty()) {
        auto *empty = new TextItem(m_listBox);
        empty->setPixelSize(Theme::smallFontSize);
        empty->setColor(Theme::textMuted);
        empty->setText(bt->powered
                           ? QStringLiteral("No devices — hit Scan.")
                           : QStringLiteral("Bluetooth is off."));
        ll->addWidget(empty);
        empty->show();
    } else {
        for (const BluetoothState::Device &d : bt->devices) {
            auto *row = new DeviceRow(d, m_listBox);
            ll->addWidget(row);
            row->show();
        }
    }

    if (isVisible())
        relayout();
}
