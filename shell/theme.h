/* Theme constants (Tokyo Night). panelHeight is also the dock's exclusive
 * zone, which the window manager reserves, so it is the single place the
 * reserved strip is defined -- nothing in its config needs to agree with it. */
#ifndef FANSHELL_QS_THEME_H
#define FANSHELL_QS_THEME_H

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRectF>
#include <QStringList>

namespace Theme {

inline const QColor bg           ("#1a1b26");
inline const QColor surface      ("#32344a");
inline const QColor surfaceHover ("#444b6a");
inline const QColor border       ("#444b6a");
inline const QColor text         ("#dddddd");
inline const QColor textMuted    ("#787c99");
inline const QColor accent       ("#7dcfff");  /* cyan accent */
inline const QColor selected     ("#7dcfff");  /* cyan selection */
inline const QColor urgent       ("#f7768e");
inline const QColor transparent  (0, 0, 0, 0);
inline const QColor textStrong   ("#c0caf5");
inline const QColor danger       ("#f7768e");  /* red */
inline const QColor success      ("#9ece6a");  /* green */
inline const QColor warning      ("#e0af68");  /* yellow */

inline const QColor black        ("#32344a");
inline const QColor red          ("#f7768e");
inline const QColor green        ("#9ece6a");
inline const QColor yellow       ("#e0af68");
inline const QColor blue         ("#7aa2f7");
inline const QColor magenta      ("#ad8ee6");
inline const QColor cyan         ("#0db9d7");
inline const QColor white        ("#787c99");

inline const QColor brightblack  ("#444b6a");
inline const QColor brightred    ("#ff7a93");
inline const QColor brightgreen  ("#b9f27c");
inline const QColor brightyellow ("#ff9e64");
inline const QColor brightblue   ("#7da6ff");
inline const QColor brightmagenta("#bb9af7");
inline const QColor brightcyan   ("#449dab");
inline const QColor brightwhite  ("#acb0d0");

/* Tag states: active = cyan, occupied (has windows) = white,
 * empty/inactive = gray, urgent = Theme::urgent. */
inline const QColor tagActive   ("#7dcfff");
inline const QColor tagOccupied ("#dddddd");
inline const QColor tagEmpty    ("#565f89");

inline const char *const fontFamily = "JetBrainsMono Nerd Font Propo";

/* The tray host is not configured here -- it follows X's primary output;
 * see WmState::primaryMonitor(). The launcher follows focus. The tag row
 * comes from the window manager (oxwm's tags, qtile's groups). */

inline constexpr int panelHeight         = 28;
/* Background only. 204 (the line below) matches Alacritty's
 * window.opacity = 0.80. */
/*inline constexpr int panelOpacity        = 204; 0 transparent .. 255 opaque */
inline constexpr int panelOpacity        = 255; /* 0 transparent .. 255 opaque */
inline constexpr int panelPadding        = 8;
inline constexpr int rowSpacing          = 8;
inline constexpr int compactSpacing      = 2;
inline constexpr int listSpacing         = 4;
inline constexpr int sectionSpacing      = 12;
inline constexpr int popupMargin         = 14;
inline constexpr int popupSpacing        = 10;
inline constexpr int radius              = 4;
inline constexpr int smallRadius         = 3;
inline constexpr int workspaceButtonSize = 22;
/* Workspaces are bubbles: a dot each, stretched to a pill for the one shown. */
inline constexpr int workspaceBubbleSize  = 12;
inline constexpr int workspaceBubbleWidth = 30;
inline constexpr int trayItemSize        = 24;
inline constexpr int trayIconSize        = 18;
inline constexpr int pillHeight          = 22;
inline constexpr int buttonHeight        = 28;
inline constexpr int chipHeight          = 24;
inline constexpr int iconSize            = 18;
inline constexpr int tinyFontSize        = 10;
inline constexpr int smallFontSize       = 12;
inline constexpr int panelFontSize       = 13;
inline constexpr int bodyFontSize        = 14;
inline constexpr int titleFontSize       = 16;
inline constexpr int maxTitleWidth       = 420;
/* Toast card opacity. Needs a compositor (picom) to show through at all; a
 * blur rule there decides what shows. */
inline constexpr int notificationOpacity = 170; /* 0 transparent .. 255 opaque */
/* Client-painted toast shadow, so toasts look the same whether or not the
 * compositor draws shadows. */
inline constexpr int notificationShadowAlpha = 36;

inline QFont font(int pixelSize, bool bold = false)
{
    QFont f(QString::fromUtf8(fontFamily));
    f.setPixelSize(pixelSize);
    f.setBold(bold);
    return f;
}

/* Paint the equivalent of a QML Rectangle: antialiased rounded rect with the
 * border drawn fully inside the item's bounds (QML border semantics). */
inline void paintRect(QPainter &p, const QRectF &r, const QColor &fill,
                      qreal radius = 0, const QColor &borderColor = QColor(),
                      qreal borderWidth = 0)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    if (fill.alpha() > 0) {
        p.setBrush(fill);
        if (radius > 0)
            p.drawRoundedRect(r, radius, radius);
        else
            p.drawRect(r);
    }
    if (borderWidth > 0 && borderColor.isValid() && borderColor.alpha() > 0) {
        QPen pen(borderColor, borderWidth);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF br = r.adjusted(borderWidth / 2.0, borderWidth / 2.0,
                                     -borderWidth / 2.0, -borderWidth / 2.0);
        const qreal ir = qMax<qreal>(0, radius - borderWidth / 2.0);
        if (radius > 0)
            p.drawRoundedRect(br, ir, ir);
        else
            p.drawRect(br);
    }
    p.restore();
}

} /* namespace Theme */

#endif
