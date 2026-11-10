#include "wallpaperimage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>

#include "paths.h"

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

/* The protocol version of fanvid's command line, stdin and stdout this
 * shell speaks; see fanvid-qs's README. */
constexpr int fanvidProtocol = 1;

} /* namespace */

QString Wallpapers::fanvid()
{
    static QMutex lock;
    static bool looked = false;
    static QString found;
    QMutexLocker locker(&lock);
    if (looked)
        return found;
    looked = true;
    QString path = Paths::setting(QStringLiteral("wallpaper.player"));
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(QStringLiteral("fanvid"));
    if (path.isEmpty())
        return found;
    QProcess p;
    p.start(path, {QStringLiteral("--protocol")});
    const bool ran = p.waitForFinished(3000) && p.exitStatus() == QProcess::NormalExit
                     && p.exitCode() == 0;
    const int version = ran ? p.readAllStandardOutput().trimmed().toInt() : 0;
    if (version == fanvidProtocol)
        found = path;
    else if (ran)
        qWarning("fanshell-qs: %s speaks protocol %d, not %d; videos are not wallpapers",
                 qPrintable(path), version, fanvidProtocol);
    else
        qWarning("fanshell-qs: %s does not run; videos are not wallpapers", qPrintable(path));
    return found;
}

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
    const QString player = fanvid();
    if (player.isEmpty())
        return QString();
    QDir().mkpath(QFileInfo(cached).absolutePath());
    /* fanvid writes it aside and renames it, so a half-written frame is
     * never read. */
    QProcess p;
    p.start(player, {QStringLiteral("still"), path, cached});
    if (!p.waitForFinished(30000) || p.exitStatus() != QProcess::NormalExit
        || p.exitCode() != 0) {
        qWarning("fanshell-qs: fanvid cannot read %s: %s", qPrintable(path),
                 p.readAllStandardError().trimmed().constData());
        p.kill();
        return QString();
    }
    return QFileInfo::exists(cached) ? cached : QString();
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
