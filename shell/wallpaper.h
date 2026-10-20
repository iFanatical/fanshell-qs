/* Wallpaper picker, backed by feh.
 *
 * A fullscreen overlay in the launcher's mould: a centred box holding a grid
 * of thumbnails, keyboard-navigable, dismissed with Escape or a click outside
 * the box. Raised over IPC so it can be bound to a key:
 *
 *     fanshell-qs-shell ipc call wallpaper toggle|show|hide
 *
 * `feh --no-fehbg --bg-<mode> <path>` sets the root window background on
 * every output. The choice is recorded in wallpaper.conf (Paths::configDir()),
 * which is both how it survives a reboot -- the shell re-applies it at
 * startup -- and how the picker knows which thumbnail to mark as current. An
 * old ~/.fehbg is imported once, when wallpaper.conf does not exist yet.
 *
 * A pick cross-fades from the old wallpaper to the new one (not the restore
 * at startup). Each output is covered by a window at the bottom of the stack
 * showing the old root pixmap; feh sets the new one underneath it, and the
 * window fades from the old pixmap to feh's before going away, so both ends
 * are exactly what feh draws, whatever the mode. It is drawing that picom
 * composites, so it stalls nothing. In shell.conf, wallpaper.fade=off turns
 * it off and wallpaper.fade_ms sets its length (default 500).
 *
 * The directory is scanned NON-recursively on purpose -- a `backup/` subtree
 * of old wallpapers lives under it that has no business in the picker. */
#ifndef FANSHELL_QS_WALLPAPER_H
#define FANSHELL_QS_WALLPAPER_H

#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

struct Wallpaper {
    QString path;
    QString name; /* basename without the extension */
    QPixmap thumb;
    bool decoded = false;
};

class WallpaperWindow;
class WallpaperFade;

class WallpaperPicker : public QObject {
    Q_OBJECT
public:
    static WallpaperPicker *instance();

    /* Directory scanned for wallpapers: wallpaper.dir in shell.conf, else
     * ~/Pictures/wallpapers. */
    static QString wallpaperDir();
    /* feh --bg-<mode> for a new pick: wallpaper.mode in shell.conf, one of
     * fill (default), center, max, scale or tile. */
    static QString mode();

    QVector<Wallpaper> entries;
    int selected = 0;
    /* Path wallpaper.conf records as displayed, so the picker can mark it. */
    QString current;

    void rescan();
    void refreshCurrent();
    void apply(int index);
    /* Re-apply the saved wallpaper; done once at startup. */
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
    /* `remember` saves wallpaper.conf once feh has succeeded; `done` gets
     * whether it did. */
    void runFeh(const QString &path, const QString &feMode, bool remember,
                std::function<void(bool)> done = {});
    void save(const QString &path, const QString &feMode);

    static constexpr const char *stateName = "wallpaper.conf";
    /* Decodes one pending thumbnail per event-loop turn. Decoding all of them
     * at once costs a few hundred ms even with scaled reads, which would be a
     * visible hitch; spreading it keeps the UI responsive and, since this
     * starts at construction, they are normally ready before the picker is
     * first opened. */
    void decodeNext();

    WallpaperWindow *m_win;
    WallpaperFade *m_fade;
    QTimer m_decoder;
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

    WallpaperPicker *m_p;
    WallpaperGrid *m_grid;
    /* Configured before its first map and re-placed when the picker moves
     * output -- see WallpaperWindow::openOn(). */
    bool m_configured = false;
    QPointer<QScreen> m_screen;
};

#endif
