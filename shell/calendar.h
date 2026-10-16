/* The centered bar clock and the calendar popup it opens. */
#ifndef FANSHELL_QS_CALENDAR_H
#define FANSHELL_QS_CALENDAR_H

#include <QDate>
#include <QWidget>

#include "popup.h"
#include "widgets.h"

class CalendarPopup;
class QTimer;

class ClockWidget : public BarPill {
    Q_OBJECT
public:
    explicit ClockWidget(QWidget *parent = nullptr);

private:
    void tick();
    void scheduleNextMinute();

    CalendarPopup *m_popup;
    QTimer *m_timer;
};

/* Month grid: 7 columns of width/7; header row of Su..Sa; a 24px circle marks
 * today. ‹ › hotspots (empty glyphs in the current theme) flip months. */
class CalendarGrid : public QWidget {
    Q_OBJECT
public:
    explicit CalendarGrid(QWidget *parent = nullptr);

    void setMonth(const QDate &today, int monthOffset);
    int contentHeight(int width) const;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    struct Cell { int day = 0; bool cur = false; bool today = false; };
    QVector<Cell> cells() const;

    QDate m_today;
    int m_offset = 0;
};

class CalendarPopup : public ShellPopup {
    Q_OBJECT
public:
    explicit CalendarPopup(QWidget *anchor);

    void openPopup() override;

public:
    /* Called at each minute boundary so `today` rolls over at midnight. */
    void updateToday();

protected:
    void mousePressEvent(QMouseEvent *) override;

private:
    QDate viewDate() const;
    void refresh();

    QDate m_today;

    int m_offset = 0;
    CalendarGrid *m_grid;
    TextItem *m_month;
};

#endif
