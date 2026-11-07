/* Wallpapers, drawn by the shell itself, and their picker.
 *
 * A fullscreen overlay in the launcher's mould: a centred box holding a grid
 * of thumbnails, keyboard-navigable, dismissed with Escape or a click outside
 * the box. Raised over IPC so it can be bound to a key:
 *
 *     fanshell-qs-shell ipc call wallpaper toggle|show|hide
 *
 * Each output gets its image laid out in one of feh's modes (fill, center,
 * max, scale, tile), decoded off the GUI thread, and the result becomes the
 * root window's background pixmap, published as _XROOTPMAP_ID and
 * ESETROOT_PMAP_ID for picom and the like. The pixmap is made on an X
 * connection of its own that closes with RetainPermanent, so it outlives the
 * shell; the next wallpaper frees it, as feh and hsetroot do.
 *
 * The picker's "Apply to" drop-down sets the wallpaper of every output or of
 * one. wallpaper.conf (Paths::configDir()) records the choice: path= and
 * mode= for every output, and path.<output>= and mode.<output>= for one that
 * differs. The shell draws it again at startup and whenever the outputs
 * change. An old ~/.fehbg is imported once, when wallpaper.conf does not
 * exist yet.
 *
 * A pick cross-fades from the old wallpaper to the new one (not the drawing
 * at startup or after an output change). Each output is covered by a window
 * at the bottom of the stack showing the old root pixmap; the new one is set
 * underneath it, and the window fades to it before going away. It is drawing
 * that picom composites, so it stalls nothing. In shell.conf,
 * wallpaper.fade=off turns it off and wallpaper.fade_ms sets its length
 * (default 500).
 *
 * On Wayland (Hyprland) there is no root window: each output gets a surface
 * of its own on the background layer, which shows its image and cross-fades
 * to the next one itself, so no wallpaper daemon (awww, hyprpaper) is
 * needed -- or wanted, as it would draw over or under this one.
 *
 * Videos (mp4, webm, mkv, mov, m4v) are wallpapers too. On Wayland, built
 * with libmpv, they play looped and silent in the background layer
 * (fanshell-qs-wallpaper-video), paused while a fullscreen window covers the output or
 * the screen is locked; wallpaper.video=off in shell.conf shows them still.
 * Everywhere else -- the picker's thumbnail, the lock screen, the root
 * window on X11, and a layer before its first frame -- a video is its first
 * frame, which ffmpeg extracts once into ~/.cache/fanshell-qs/wallpapers.
 *
 * The directory is scanned NON-recursively on purpose -- a `backup/` subtree
 * of old wallpapers lives under it that has no business in the picker. */
#ifndef FANSHELL_QS_WALLPAPER_H
#define FANSHELL_QS_WALLPAPER_H

#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "wallpaperimage.h"

namespace Wallpapers {

struct Choice {
    QString path;
    QString mode;
};

/* wallpaper.conf: one choice for every output, and those that differ. */
struct Config {
    Choice all;
    QHash<QString, Choice> outputs;

    Choice forOutput(const QString &output) const;
    static Config load();
    void save() const;
};

/* Whether videos play (wallpaper.video in shell.conf, and the player on
 * Wayland), rather than showing their first frame. */
bool videoPlays();

} /* namespace Wallpapers */

struct Wallpaper {
    QString path;
    QString name; /* basename without the extension */
    QPixmap thumb;
    bool decoded = false;
};

class WallpaperWindow;
class WallpaperFade;
class WallpaperLayer;
class QScreen;

class WallpaperPicker : public QObject {
    Q_OBJECT
public:
    static WallpaperPicker *instance();

    /* Directory scanned for wallpapers: wallpaper.dir in shell.conf, else
     * ~/Pictures/wallpapers. */
    static QString wallpaperDir();
    /* The mode for a new pick: wallpaper.mode in shell.conf, one of fill
     * (default), center, max, scale or tile. */
    static QString mode();

    QVector<Wallpaper> entries;
    int selected = 0;
    /* The output a pick applies to, by name; empty for every output. */
    QString target;

    /* Whether `path` is showing on the target (on any output, for all). */
    bool isCurrent(const QString &path) const;
    /* The outputs connected now, by name, for the "Apply to" list. */
    static QStringList outputNames();

    void rescan();
    void apply(int index);
    /* Draw wallpaper.conf again; done at startup and after output changes. */
    void restore();
    void move(int delta);
    void syncSelectionToCurrent();

    void openPicker();
    void togglePicker();
    void hidePicker();

    static constexpr int columns = 4;
    /* 16:9, comfortably legible without making the box fill the screen. */
    static constexpr int thumbW = 288;
    static constexpr int thumbH = 162;
    /* Room under the thumbnail for the filename. */
    static constexpr int cellPad = 8;
    static constexpr int labelH = 20;
    static int cellW() { return thumbW + 2 * cellPad; }
    static int cellH() { return thumbH + labelH + 3 * cellPad; }

signals:
    void changed();
    void selectionChanged();

private:
    explicit WallpaperPicker(QObject *parent = nullptr);
    /* Lay `conf` out on every output off the GUI thread, then make it the
     * root background -- cross-fading when `fade` -- and keep it (saving
     * wallpaper.conf when `save`). Left as it was if an image cannot be
     * read. */
    void draw(const Wallpapers::Config &conf, bool fade, bool save);
    /* draw() on Wayland: an image per output, into its background layer. */
    void drawLayers(const Wallpapers::Config &conf, bool fade, bool save);

    /* Decodes one pending thumbnail per event-loop turn. Decoding all of them
     * at once costs a few hundred ms even with scaled reads, which would be a
     * visible hitch; spreading it keeps the UI responsive and, since this
     * starts at construction, they are normally ready before the picker is
     * first opened. */
    void decodeNext();
    /* A video's first frame is being extracted for its thumbnail. */
    bool m_extracting = false;

    Wallpapers::Config m_conf;
    WallpaperWindow *m_win;
    WallpaperFade *m_fade;
    /* Wayland: each output's background surface. */
    QHash<QScreen *, WallpaperLayer *> m_layers;
    QTimer m_decoder;
    /* Outputs added, removed or moved: draw again once RandR settles. */
    QTimer m_outputsChanged;
    /* Bumped per draw(), so only the latest one's result is used. */
    int m_generation = 0;
};

/* The results grid: fixed columns, scrolls when it overflows. */
class WallpaperGrid : public QWidget {
    Q_OBJECT
public:
    explicit WallpaperGrid(WallpaperPicker *p, QWidget *parent = nullptr);

    int contentHeight() const;
    void ensureVisible();

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    int indexAt(const QPoint &p) const;
    void clampScroll();

    WallpaperPicker *m_p;
    int m_scroll = 0;
};

class ChoiceList;
class Selector;
class TextItem;

class WallpaperWindow : public QWidget {
    Q_OBJECT
public:
    explicit WallpaperWindow(WallpaperPicker *p);

    void openOn(QScreen *screen);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    void layoutBox();
    void syncTarget();

    WallpaperPicker *m_p;
    WallpaperGrid *m_grid;
    TextItem *m_targetLabel;
    Selector *m_target;
    ChoiceList *m_list;
    /* Configured before its first map and re-placed when the picker moves
     * output -- see WallpaperWindow::openOn(). */
    bool m_configured = false;
    QPointer<QScreen> m_screen;
};

#endif
