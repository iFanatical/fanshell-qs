#include "widgets.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

/* ---------------------------------------------------------------- TextItem */

TextItem::TextItem(QWidget *parent) : QWidget(parent)
{
    QSizePolicy sp(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setSizePolicy(sp);
}

void TextItem::setText(const QString &t, bool updateLayout)
{
    if (m_text == t)
        return;
    m_text = t;
    if (updateLayout)
        updateGeometry();
    update();
}

void TextItem::setColor(const QColor &c)
{
    if (m_color == c)
        return;
    m_color = c;
    update();
}

void TextItem::setPixelSize(int px)
{
    m_pixelSize = px;
    updateGeometry();
    update();
}

void TextItem::setBold(bool b)
{
    if (m_bold == b)
        return;
    m_bold = b;
    updateGeometry();
    update();
}

void TextItem::setElide(bool e)
{
    m_elide = e;
    if (e)
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    updateGeometry();
}

void TextItem::setHAlign(Qt::Alignment a)
{
    m_halign = a;
    update();
}

QSize TextItem::sizeHint() const
{
    /* QML Text implicitWidth is fractional; take the ceiling like the scene
     * graph's device-pixel snap. */
    const QFontMetricsF fm(Theme::font(m_pixelSize, m_bold));
    return QSize(qCeil(fm.horizontalAdvance(m_text)), qCeil(fm.height()));
}

QSize TextItem::minimumSizeHint() const
{
    if (m_elide)
        return QSize(0, sizeHint().height());
    return sizeHint();
}

void TextItem::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setFont(Theme::font(m_pixelSize, m_bold));
    p.setPen(m_color);
    QString t = m_text;
    if (m_elide) {
        const QFontMetrics fm(p.font());
        if (fm.horizontalAdvance(t) > width())
            t = fm.elidedText(t, Qt::ElideRight, width());
    }
    p.drawText(rect(), int(m_halign) | Qt::AlignVCenter, t);
}

/* ----------------------------------------------------------------- BarPill */

BarPill::BarPill(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(Theme::pillHeight);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void BarPill::setIcon(const QString &icon)
{
    if (m_icon == icon)
        return;
    m_icon = icon;
    updateGeometry();
    update();
}

void BarPill::setLabel(const QString &label)
{
    if (!m_useRich && m_label == label)
        return;
    m_label = label;
    m_useRich = false;
    m_rich.clear();
    updateGeometry();
    update();
}

void BarPill::setRichSegments(const QVector<Segment> &segs)
{
    m_rich = segs;
    m_useRich = true;
    m_label.clear();
    updateGeometry();
    update();
}

void BarPill::setTint(const QColor &c)
{
    if (m_tint == c)
        return;
    m_tint = c;
    update();
}

void BarPill::setActive(bool a)
{
    if (m_active == a)
        return;
    m_active = a;
    update();
}

void BarPill::setLarge(bool l)
{
    if (m_large == l)
        return;
    m_large = l;
    setFixedHeight(m_large ? Theme::pillHeight * 2 : Theme::pillHeight);
    updateGeometry();
    update();
}

qreal BarPill::contentWidth() const
{
    const int fsize = m_large ? Theme::panelFontSize * 2 : Theme::panelFontSize;
    const QFontMetricsF fm(Theme::font(fsize));
    qreal w = 0;
    bool hasIcon = !m_icon.isEmpty();
    if (hasIcon)
        w += fm.horizontalAdvance(m_icon);
    qreal labelW = 0;
    if (m_useRich) {
        for (const Segment &s : m_rich)
            labelW += fm.horizontalAdvance(s.text);
    } else if (!m_label.isEmpty()) {
        labelW = fm.horizontalAdvance(m_label);
    }
    if (hasIcon && labelW > 0)
        w += m_large ? 10 : 5;       /* RowLayout spacing: 5 */
    w += labelW;
    return w;
}

QSize BarPill::sizeHint() const
{
    const int pad = m_large ? 32 : 16;
    const int h = m_large ? Theme::pillHeight * 2 : Theme::pillHeight;
    return QSize(qCeil(contentWidth() + pad), h);
}

void BarPill::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const bool hl = highlighted();
    Theme::paintRect(p, rect(), hl ? Theme::surfaceHover : Theme::surface,
                     m_large ? Theme::radius * 2 : Theme::radius, Theme::accent,
                     m_active ? (m_large ? 2 : 1) : 0);

    const int fsize = m_large ? Theme::panelFontSize * 2 : Theme::panelFontSize;
    p.setFont(Theme::font(fsize));
    const QFontMetricsF fm(p.font());
    qreal x = (width() - contentWidth()) / 2.0;
    const QColor base = hl ? Theme::tagActive : m_tint;
    if (!m_icon.isEmpty()) {
        p.setPen(base);
        p.drawText(QRectF(x, 0, fm.horizontalAdvance(m_icon), height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_icon);
        x += fm.horizontalAdvance(m_icon);
        if ((m_useRich && !m_rich.isEmpty()) || (!m_useRich && !m_label.isEmpty()))
            x += m_large ? 10 : 5;
    }
    if (m_useRich) {
        for (const Segment &s : m_rich) {
            const QColor c = s.color.isValid() ? s.color : base;
            p.setPen(c);
            const qreal w = fm.horizontalAdvance(s.text);
            p.drawText(QRectF(x, 0, w, height()),
                       Qt::AlignLeft | Qt::AlignVCenter, s.text);
            x += w;
        }
    } else if (!m_label.isEmpty()) {
        p.setPen(base);
        p.drawText(QRectF(x, 0, fm.horizontalAdvance(m_label), height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_label);
    }
}

void BarPill::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
    emit hoverChanged();
}

void BarPill::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
    emit hoverChanged();
}

void BarPill::mousePressEvent(QMouseEvent *e)
{
    m_pressedButton = e->button();
}

void BarPill::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == m_pressedButton && rect().contains(e->pos())) {
        if (e->button() == Qt::RightButton)
            emit rightClicked();
        else if (e->button() == Qt::LeftButton)
            emit clicked();
    }
    m_pressedButton = Qt::NoButton;
}

void BarPill::wheelEvent(QWheelEvent *e)
{
    emit scrolled(e->angleDelta().y() > 0 ? 1 : -1);
}

/* ------------------------------------------------------------- ShellButton */

ShellButton::ShellButton(const QString &label, QWidget *parent)
    : QWidget(parent), m_label(label)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(Theme::buttonHeight);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void ShellButton::setLabel(const QString &l)
{
    if (m_label == l)
        return;
    m_label = l;
    updateGeometry();
    update();
}

void ShellButton::setDanger(bool d)
{
    if (m_danger == d)
        return;
    m_danger = d;
    update();
}

void ShellButton::setActive(bool a)
{
    if (m_active == a)
        return;
    m_active = a;
    update();
}

QSize ShellButton::sizeHint() const
{
    const QFontMetricsF fm(Theme::font(Theme::smallFontSize, true));
    return QSize(qCeil(fm.horizontalAdvance(m_label) + 22),
                 Theme::buttonHeight);
}

void ShellButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (!isEnabled())
        p.setOpacity(0.5);
    const QColor fill = (m_hover && isEnabled()) ? Theme::surfaceHover : Theme::surface;
    const QColor bord = m_danger ? Theme::danger
                                 : (m_active ? Theme::accent : Theme::border);
    Theme::paintRect(p, rect(), fill, Theme::radius, bord, 1);
    p.setFont(Theme::font(Theme::smallFontSize, true));
    p.setPen(m_danger ? Theme::danger : (m_active ? Theme::accent : Theme::text));
    p.drawText(rect(), Qt::AlignCenter, m_label);
}

void ShellButton::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}

void ShellButton::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void ShellButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        m_pressed = true;
}

void ShellButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_pressed && e->button() == Qt::LeftButton && rect().contains(e->pos())
            && isEnabled())
        emit activated();
    m_pressed = false;
}

void ShellButton::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::EnabledChange)
        update();
    QWidget::changeEvent(e);
}

/* ------------------------------------------------------------- ValueSlider */

ValueSlider::ValueSlider(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(16);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void ValueSlider::setValue(qreal v)
{
    m_value = v;
    update();
}

void ValueSlider::setFillColor(const QColor &c)
{
    if (m_fill == c)
        return;
    m_fill = c;
    update();
}

static qreal clamp01(qreal v) { return qMax<qreal>(0, qMin<qreal>(1, v)); }

void ValueSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const qreal v = clamp01(m_value);
    /* track */
    const QRectF track(0, (height() - 6) / 2.0, width(), 6);
    Theme::paintRect(p, track, Theme::surface, 3);
    /* fill */
    Theme::paintRect(p, QRectF(track.x(), track.y(), v * track.width(), 6),
                     m_fill, 3);
    /* handle */
    const qreal hx = v * (width() - 12);
    Theme::paintRect(p, QRectF(hx, (height() - 12) / 2.0, 12, 12), m_fill, 6,
                     Theme::bg, 1);
}

void ValueSlider::mousePressEvent(QMouseEvent *e)
{
    m_dragging = true;
    emit moved(clamp01(e->position().x() / qMax(1, width())));
}

void ValueSlider::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging)
        emit moved(clamp01(e->position().x() / qMax(1, width())));
}

void ValueSlider::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
}

/* ------------------------------------------------------------------- HLine */

HLine::HLine(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(1);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HLine::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::border);
}
