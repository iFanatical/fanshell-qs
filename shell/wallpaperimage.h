/* A wallpaper as an image: what the shell and the video player
 * (wallpapervideo.cpp) share, so the player links none of the shell. */
#ifndef FANSHELL_QS_WALLPAPERIMAGE_H
#define FANSHELL_QS_WALLPAPERIMAGE_H

#include <QImage>
#include <QSize>
#include <QString>

namespace Wallpapers {

/* `path` laid out in a box of `size` as feh --bg-<mode> lays it out on one
 * output, black where it leaves room; null when it cannot be read. Safe off
 * the GUI thread. */
QImage render(const QString &path, const QString &mode, const QSize &size);

/* Whether `path` is a video, by its extension. */
bool isVideo(const QString &path);
/* An image of `path`: itself, or for a video its first frame, extracted
 * with ffmpeg and cached (blocking; off the GUI thread). Empty when it
 * cannot be had. */
QString still(const QString &path);
/* still() if it is already cached, without extracting anything. */
QString cachedStill(const QString &path);

} /* namespace Wallpapers */

#endif
