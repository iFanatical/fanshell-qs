#include "calendar.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

/* ------------------------------------------------------------- ClockWidget */

ClockWidget::ClockWidget(QWidget *parent) : BarPill(parent)
{
    setIcon(QStringLiteral(""));
    setTint(Theme::text);
    m_popup = new CalendarPopup(this);
    connect(this, &BarPill::clicked, m_popup, &ShellPopup::togglePopup);
    connect(m_popup, &ShellPopup::popupVisibleChanged, this,
            &BarPill::setActive);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &ClockWidget::tick);
    tick();
}

void ClockWidget::tick()
{
    /* Day/month names in English (C locale); 12-hour clock with AM/PM. */
    setLabel(QDateTime::currentDateTime().toString(
        QStringLiteral("ddd, MMM dd h:mm AP")));
    if (m_popup->isVisible())
        m_popup->updateToday();
    scheduleNextMinute();
}

void ClockWidget::scheduleNextMinute()
{
    /* Recalculate after every tick rather than repeating at 60 seconds. This
     * cannot accumulate drift and corrects itself after suspend or a clock
     * adjustment. PreciseTimer is intentional: coarse timers may fire early,
     * briefly displaying the previous minute past the visual boundary. */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int delay = int(60000 - (now % 60000));
    m_timer->start(qMax(1, delay));
}

/* ------------------------------------------------------------ CalendarGrid */

CalendarGrid::CalendarGrid(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CalendarGrid::setMonth(const QDate &today, int monthOffset)
{
    m_today = today;
    m_offset = monthOffset;
    /* the popup is a fixed 260 wide, so the padded content is always 232 */
    setFixedHeight(contentHeight(260 - 2 * Theme::popupMargin));
    update();
}

QVector<CalendarGrid::Cell> CalendarGrid::cells() const
{
    const QDate vd = QDate(m_today.year(), m_today.month(), 1)
                         .addMonths(m_offset);
    const int first = vd.dayOfWeek() % 7; /* 0 = Sunday */
    const int dim = vd.daysInMonth();
    QVector<Cell> arr;
    for (int i = 0; i < first; i++)
        arr.push_back({});
    for (int d = 1; d <= dim; d++) {
        const bool isToday = m_offset == 0 && d == m_today.day()
                             && vd.month() == m_today.month()
                             && vd.year() == m_today.year();
        arr.push_back({d, true, isToday});
    }
    while (arr.size() % 7 != 0)
        arr.push_back({});
    return arr;
}

int CalendarGrid::contentHeight(int w) const
{
    const qreal cell = w / 7.0;
    const int rows = cells().size() / 7;
    const int dowH = QFontMetrics(Theme::font(Theme::tinyFontSize, true))
                         .height();
    return qRound(dowH + rows * cell);
}

void CalendarGrid::paintEvent(QPaintEvent *)
{
    static const char *dows[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    QPainter p(this);
    const qreal cell = width() / 7.0;

    p.setFont(Theme::font(Theme::tinyFontSize, true));
    const int dowH = p.fontMetrics().height();
    p.setPen(Theme::textMuted);
    for (int i = 0; i < 7; i++)
        p.drawText(QRectF(i * cell, 0, cell, dowH), Qt::AlignCenter,
                   QString::fromLatin1(dows[i]));

    const QVector<Cell> arr = cells();
    for (int i = 0; i < arr.size(); i++) {
        const Cell &c = arr[i];
        const int row = i / 7, col = i % 7;
        const QRectF r(col * cell, dowH + row * cell, cell, cell);
        const QRectF circle(r.x() + (r.width() - 24) / 2.0,
                            r.y() + (r.height() - 24) / 2.0, 24, 24);
        if (c.today)
            Theme::paintRect(p, circle, Theme::accent, 12);
        if (c.day > 0) {
            p.setFont(Theme::font(Theme::smallFontSize, c.today));
            p.setPen(c.today ? Theme::bg
                             : (c.cur ? Theme::text : Theme::textMuted));
            p.drawText(circle, Qt::AlignCenter, QString::number(c.day));
        }
    }
}

/* ----------------------------------------------------------- CalendarPopup */

CalendarPopup::CalendarPopup(QWidget *anchor) : ShellPopup(anchor, 260)
{
    /* The clock is centred in the bar, so the calendar is too. */
    setAnchorCentered(true);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(Theme::popupMargin, Theme::popupMargin,
                            Theme::popupMargin, Theme::popupMargin);
    lay->setSpacing(Theme::popupSpacing);

    /* header: ‹  Month Year  › (nav glyphs are empty strings in the current
     * theme; the hotspots remain — see mousePressEvent) */
    auto *header = new QWidget(this);
    header->setFixedHeight(24);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);
    m_month = new TextItem(header);
    m_month->setPixelSize(Theme::bodyFontSize);
    m_month->setBold(true);
    m_month->setColor(Theme::accent);
    m_month->setHAlign(Qt::AlignHCenter);
    hl->addStretch(1);
    hl->addWidget(m_month, 0, Qt::AlignVCenter);
    hl->addStretch(1);
    lay->addWidget(header);

    m_grid = new CalendarGrid(this);
    lay->addWidget(m_grid);
    lay->addStretch(0);

    m_today = QDate::currentDate();
    refresh();
}

QDate CalendarPopup::viewDate() const
{
    return QDate(m_today.year(), m_today.month(), 1).addMonths(m_offset);
}

void CalendarPopup::refresh()
{
    m_month->setText(viewDate().toString(QStringLiteral("MMMM yyyy")));
    m_grid->setMonth(m_today, m_offset);
    relayout();
}

void CalendarPopup::openPopup()
{
    m_offset = 0;
    m_today = QDate::currentDate();
    refresh();
    ShellPopup::openPopup();
}

void CalendarPopup::updateToday()
{
    const QDate d = QDate::currentDate();
    if (d != m_today) {
        m_today = d;
        refresh();
    }
}

void CalendarPopup::mousePressEvent(QMouseEvent *e)
{
    /* ‹ › hotspots: the 12px-wide zones around the (empty) nav glyphs at the
     * left/right edges of the header row. */
    const int m = Theme::popupMargin;
    const QRectF header(m, m, width() - 2 * m, 24);
    if (e->position().y() >= header.top() - 6
            && e->position().y() <= header.bottom() + 6) {
        if (e->position().x() <= header.left() + 6) {
            m_offset--;
            refresh();
            return;
        }
        if (e->position().x() >= header.right() - 6) {
            m_offset++;
            refresh();
            return;
        }
    }
    ShellPopup::mousePressEvent(e);
}
