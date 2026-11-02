/* A video wallpaper's playback: libmpv, rendered through its OpenGL render
 * API into the wallpaper layer (wallpaper.cpp), looped and silent, decoded
 * on the GPU where it can be (hwdec=auto-safe).
 *
 * Built when the shell is built for Wayland and libmpv is found
 * (FANSHELL_QS_HAVE_MPV); without it, and on X11, a video wallpaper shows
 * its first frame as a still image (Wallpapers::still()). */
#ifndef FANSHELL_QS_WALLPAPERVIDEO_H
#define FANSHELL_QS_WALLPAPERVIDEO_H

#include <QSize>
#include <QString>

#include <memory>

class QWidget;
struct mpv_handle;
struct mpv_render_context;

class VideoPlayer {
public:
    /* Whether video wallpapers can play here: built with libmpv and running
     * on Wayland. */
    static bool available();

    /* Starts `path`, laid out as `mode` (feh's modes, as for images), and
     * repaints `view` whenever a new frame is due. `view` must outlive the
     * player. */
    VideoPlayer(QWidget *view, const QString &path, const QString &mode);
    /* releaseGL() first, with the view's GL context current. */
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    const QString &path() const { return m_path; }
    const QString &mode() const { return m_mode; }

    /* Draws the current frame into framebuffer `fbo` of `size` device
     * pixels, with the view's GL context current. False, drawing nothing,
     * until the first frame has been decoded (or when the file cannot be
     * played), so the caller shows the still meanwhile. */
    bool render(int fbo, const QSize &size);

    void setPaused(bool paused);

    /* Frees the render context; with the view's GL context current. */
    void releaseGL();

private:
    void drainEvents();

    QWidget *m_view;
    QString m_path;
    QString m_mode;
    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_gl = nullptr;
    bool m_frame = false;
    bool m_failed = false;
    bool m_paused = false;
    /* Reset on destruction; mpv's queued wakeups check it. */
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

#endif
