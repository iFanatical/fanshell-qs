/* A wallpaper as an image, and fanvid, the optional add-on
 * (github.com/iFanatical/fanvid-qs) that plays video wallpapers and gives
 * their first frames. */
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
/* fanvid: wallpaper.player in shell.conf, else fanvid on PATH, when it
 * speaks the protocol this shell knows; else empty, and videos are not
 * wallpapers. Looked for once. */
QString fanvid();
/* An image of `path`: itself, or for a video its first frame, extracted
 * with fanvid and cached (blocking; off the GUI thread). Empty when it
 * cannot be had; without fanvid, only one already cached. */
QString still(const QString &path);
/* still() if it is already cached, without extracting anything. */
QString cachedStill(const QString &path);

} /* namespace Wallpapers */

#endif
