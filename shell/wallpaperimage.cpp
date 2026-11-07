#include "wallpaperimage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QProcess>

bool Wallpapers::isVideo(const QString &path)
{
    static const QStringList exts{QStringLiteral("mp4"), QStringLiteral("webm"),
                                  QStringLiteral("mkv"), QStringLiteral("mov"),
                                  QStringLiteral("m4v")};
    return exts.contains(QFileInfo(path).suffix().toLower());
}

namespace {

/* Where still() keeps `path`'s first frame: named for the file and its
 * size and time, so an edited video gets a new one. */
QString stillCachePath(const QString &path)
{
    const QFileInfo fi(path);
    QString cache = qEnvironmentVariable("XDG_CACHE_HOME");
    if (cache.isEmpty())
        cache = QDir::homePath() + QStringLiteral("/.cache");
    const QByteArray key = QStringLiteral("%1\n%2\n%3")
                               .arg(fi.absoluteFilePath())
                               .arg(fi.size())
                               .arg(fi.lastModified().toMSecsSinceEpoch())
                               .toUtf8();
    return cache + QStringLiteral("/fanshell-qs/wallpapers/")
           + QString::fromLatin1(
                 QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex())
           + QStringLiteral(".png");
}

} /* namespace */

QString Wallpapers::cachedStill(const QString &path)
{
    if (!isVideo(path))
        return path;
    const QString cached = stillCachePath(path);
    return QFileInfo::exists(cached) ? cached : QString();
}

QString Wallpapers::still(const QString &path)
{
    if (!isVideo(path))
        return path;
    const QString cached = stillCachePath(path);
    if (QFileInfo::exists(cached))
        return cached;
    QDir().mkpath(QFileInfo(cached).absolutePath());
    /* Written aside and renamed, so a half-written frame is never read. */
    const QString part = cached + QStringLiteral(".part");
    QProcess ffmpeg;
    ffmpeg.start(QStringLiteral("ffmpeg"),
                 {QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
                  QStringLiteral("-y"), QStringLiteral("-i"), path,
                  QStringLiteral("-frames:v"), QStringLiteral("1"),
                  QStringLiteral("-f"), QStringLiteral("image2"),
                  QStringLiteral("-c:v"), QStringLiteral("png"), part});
    if (!ffmpeg.waitForFinished(30000) || ffmpeg.exitStatus() != QProcess::NormalExit
        || ffmpeg.exitCode() != 0) {
        if (ffmpeg.error() == QProcess::FailedToStart)
            qWarning("fanshell-qs: ffmpeg is needed for a video wallpaper's still");
        else
            qWarning("fanshell-qs: ffmpeg cannot read %s: %s", qPrintable(path),
                     ffmpeg.readAllStandardError().trimmed().constData());
        ffmpeg.kill();
        QFile::remove(part);
        return QString();
    }
    if (!QFile::rename(part, cached)) {
        QFile::remove(part);
        return QFileInfo::exists(cached) ? cached : QString();
    }
    return cached;
}

QImage Wallpapers::render(const QString &source, const QString &mode, const QSize &size)
{
    if (source.isEmpty() || size.isEmpty())
        return QImage();
    const QString path = still(source);
    if (path.isEmpty())
        return QImage();
    QImageReader reader(path);
    reader.setAutoTransform(true);
    /* Have the decoder shrink what will be shrunk anyway; much cheaper for
     * large JPEGs than decoding at full size. */
    const QSize raw = reader.size();
    if (raw.isValid() && (mode == QLatin1String("fill") || mode == QLatin1String("max")
                          || mode == QLatin1String("scale"))) {
        QSize shown = raw;
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            shown.transpose();
        const double fx = double(size.width()) / shown.width();
        const double fy = double(size.height()) / shown.height();
        const double f = mode == QLatin1String("max") ? qMin(fx, fy) : qMax(fx, fy);
        if (f < 1.0)
            reader.setScaledSize(QSize(qMax(1, int(std::ceil(raw.width() * f))),
                                       qMax(1, int(std::ceil(raw.height() * f)))));
    }
    const QImage img = reader.read();
    if (img.isNull())
        return QImage();

    QImage out(size, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    auto centred = [&p, &size](const QImage &i) {
        p.drawImage((size.width() - i.width()) / 2, (size.height() - i.height()) / 2, i);
    };
    if (mode == QLatin1String("center"))
        centred(img);
    else if (mode == QLatin1String("tile"))
        p.fillRect(out.rect(), QBrush(img));
    else if (mode == QLatin1String("scale"))
        p.drawImage(0, 0, img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    else if (mode == QLatin1String("max"))
        centred(img.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        centred(img.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    p.end();
    return out;
}
