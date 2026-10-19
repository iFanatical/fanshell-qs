#include "lockscreen.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QHash>
#include <QImageReader>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcessEnvironment>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QWidget>
#include <QWindow>

#include <security/pam_appl.h>
#include <xcb/dpms.h>
#include <xcb/xcb.h>
/* xkb.h names a struct field `explicit`, which C++ reserves. */
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "battery.h"
#include "ipc.h"
#include "media.h"
#include "osd.h"
#include "paths.h"
#include "procutil.h"
#include "surface.h"
#include "theme.h"

namespace {

/* The locker's exit statuses, as the supervisor reads them. */
constexpr int lockerUnlocked = 0;
constexpr int lockerGrabFailed = 3;

/* comm of the supervisor and the locker, so `pkill -x fanshell-lock` finds
 * them and the restart script's match on fanshell-qs-shell does not. */
constexpr auto processName = "fanshell-lock";

/* Back to the clock after this long in the password view without input. */
constexpr int promptIdleMs = 30000;
/* The pointer hides after this long without moving. */
constexpr int cursorIdleMs = 3000;

xcb_connection_t *conn()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}

xcb_window_t rootWindow(xcb_connection_t *c)
{
    return xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
}

/* An X server answers on $DISPLAY. */
bool displayAlive()
{
    xcb_connection_t *c = xcb_connect(nullptr, nullptr);
    const bool ok = !xcb_connection_has_error(c);
    xcb_disconnect(c);
    return ok;
}

/* The PAM service to check passwords with: our own, else i3lock's. */
QByteArray pamService()
{
    for (const char *name : {"fanshell-qs-lock", "i3lock"})
        for (const char *dir : {"/etc/pam.d/", "/usr/lib/pam.d/"})
            if (QFile::exists(QString::fromLatin1(dir) + QLatin1String(name)))
                return QByteArray(name);
    return QByteArray();
}

QString glyph(char32_t c)
{
    return QString::fromUcs4(&c, 1);
}

/* The lock screen's text face; the bar's Nerd Font is kept for glyphs. */
QFont uiFont(int px, QFont::Weight weight = QFont::Normal)
{
    QFont f;
    f.setFamilies({QStringLiteral("Inter Display"), QStringLiteral("Inter"),
                   QStringLiteral("Cantarell"), QStringLiteral("Noto Sans"),
                   QStringLiteral("DejaVu Sans")});
    f.setPixelSize(px);
    f.setWeight(weight);
    return f;
}

double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

/* ------------------------------------------------------------------ PAM */

int converse(int n, const struct pam_message **msg,
             struct pam_response **resp, void *password)
{
    auto *r = static_cast<pam_response *>(calloc(size_t(n), sizeof(pam_response)));
    if (!r)
        return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF
            && msg[i]->msg_style != PAM_PROMPT_ECHO_ON)
            continue;
        r[i].resp = strdup(static_cast<const char *>(password));
        if (!r[i].resp) {
            for (int j = 0; j < i; j++)
                free(r[j].resp);
            free(r);
            return PAM_BUF_ERR;
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

/* Blocks for as long as PAM takes (a wrong password: its fail delay). */
bool authenticate(const QByteArray &service, const QByteArray &user,
                  const char *password)
{
    pam_conv conv{converse, const_cast<char *>(password)};
    pam_handle_t *h = nullptr;
    if (pam_start(service.constData(), user.constData(), &conv, &h) != PAM_SUCCESS)
        return false;
    const int rc = pam_authenticate(h, 0);
    if (rc == PAM_SUCCESS)
        pam_setcred(h, PAM_REFRESH_CRED);
    pam_end(h, rc);
    return rc == PAM_SUCCESS;
}

/* ------------------------------------------------------------ wallpaper */

/* Three passes of a box blur, each row then each column, approximate a
 * Gaussian. */
void boxBlur(QImage &img, int radius)
{
    const int w = img.width(), h = img.height();
    if (radius < 1 || w < 2 || h < 2)
        return;
    QVector<QRgb> line(qMax(w, h));
    const int span = 2 * radius + 1;
    auto pass = [&](QRgb *start, int count, int stride) {
        for (int i = 0; i < count; i++)
            line[i] = start[i * stride];
        int r = 0, g = 0, b = 0;
        for (int k = -radius; k <= radius; k++) {
            const QRgb px = line[qBound(0, k, count - 1)];
            r += qRed(px);
            g += qGreen(px);
            b += qBlue(px);
        }
        for (int i = 0; i < count; i++) {
            start[i * stride] = qRgb(r / span, g / span, b / span);
            const QRgb out = line[qBound(0, i - radius, count - 1)];
            const QRgb in = line[qBound(0, i + radius + 1, count - 1)];
            r += qRed(in) - qRed(out);
            g += qGreen(in) - qGreen(out);
            b += qBlue(in) - qBlue(out);
        }
    };
    for (int n = 0; n < 3; n++) {
        for (int y = 0; y < h; y++)
            pass(reinterpret_cast<QRgb *>(img.scanLine(y)), w, 1);
        auto *bits = reinterpret_cast<QRgb *>(img.bits());
        const int stride = img.bytesPerLine() / int(sizeof(QRgb));
        for (int x = 0; x < w; x++)
            pass(bits + x, h, stride);
    }
}

/* The saved wallpaper, decoded small: it is blurred heavily anyway. */
QImage wallpaperImage()
{
    const QString path = Paths::readConf(QStringLiteral("wallpaper.conf"))
                             .value(QStringLiteral("path"));
    if (path.isEmpty())
        return QImage();
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid())
        reader.setScaledSize(size.scaled(960, 960, Qt::KeepAspectRatio));
    QImage img = reader.read();
    return img.isNull() ? img : img.convertToFormat(QImage::Format_RGB32);
}

/* `src` filling `size` as feh --bg-fill would, blurred and dimmed. */
QPixmap blurredBackground(const QImage &src, const QSize &size)
{
    QImage out;
    if (src.isNull()) {
        out = QImage(size, QImage::Format_RGB32);
        out.fill(Theme::bg);
    } else {
        const QSize small = size.scaled(480, 480, Qt::KeepAspectRatio);
        QImage cover = src.scaled(small, Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation);
        cover = cover.copy((cover.width() - small.width()) / 2,
                           (cover.height() - small.height()) / 2,
                           small.width(), small.height())
                    .convertToFormat(QImage::Format_RGB32);
        boxBlur(cover, qMax(2, small.width() / 110));
        out = cover.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QPainter p(&out);
    p.fillRect(out.rect(), QColor(16, 17, 26, 115));
    p.end();
    return QPixmap::fromImage(out);
}

/* The account's picture, as display managers find it. */
QImage avatarImage(const QByteArray &user)
{
    const QStringList candidates{
        QDir::homePath() + QStringLiteral("/.face"),
        QDir::homePath() + QStringLiteral("/.face.icon"),
        QStringLiteral("/var/lib/AccountsService/icons/") + QString::fromLocal8Bit(user)};
    for (const QString &path : candidates) {
        QImage img(path);
        if (!img.isNull())
            return img;
    }
    return QImage();
}

QString uptimeText()
{
    QFile f(QStringLiteral("/proc/uptime"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const qint64 s = qint64(f.readAll().split(' ').value(0).toDouble());
    const qint64 days = s / 86400, hours = s % 86400 / 3600, mins = s % 3600 / 60;
    auto unit = [](qint64 n, const char *name) {
        return QStringLiteral("%1 %2%3").arg(n).arg(QLatin1String(name),
                                                 n == 1 ? "" : "s");
    };
    if (days > 0)
        return QStringLiteral("up ") + unit(days, "day") + QStringLiteral(", ")
               + unit(hours, "hour");
    if (hours > 0)
        return QStringLiteral("up ") + unit(hours, "hour") + QStringLiteral(", ")
               + unit(mins, "minute");
    return QStringLiteral("up ") + unit(mins, "minute");
}

/* Tells the running shell something, without waiting on it. */
void notifyShell(QObject *ctx, const QString &target, const QString &function,
                 std::function<void()> done = {})
{
    auto *sock = new QLocalSocket(ctx);
    auto finished = std::make_shared<bool>(false);
    auto finish = [sock, finished, done]() {
        if (*finished)
            return;
        *finished = true;
        sock->deleteLater();
        if (done)
            done();
    };
    QObject::connect(sock, &QLocalSocket::connected, sock, [sock, target, function]() {
        sock->write((target + QLatin1Char(' ') + function + QLatin1Char('\n')).toUtf8());
    });
    QObject::connect(sock, &QLocalSocket::readyRead, sock, finish);
    QObject::connect(sock, &QLocalSocket::disconnected, sock, finish);
    QObject::connect(sock, &QLocalSocket::errorOccurred, sock, finish);
    QTimer::singleShot(1000, sock, finish);
    sock->connectToServer(IpcServer::socketPath());
}

class Locker;

/* One per output: the background, and on the primary everything else. */
class LockWindow : public QWidget {
public:
    LockWindow(Locker *locker, QScreen *screen)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint
                               | Qt::X11BypassWindowManagerHint
                               | Qt::WindowStaysOnTopHint),
          m_locker(locker), m_screen(screen)
    {
        setWindowTitle(QStringLiteral("fanshell-qs-lock"));
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setCursor(Qt::ArrowCursor);
        setGeometry(screen->geometry());
    }

    QScreen *lockScreen() const { return m_screen; }

    /* The media buttons as last painted, for clicks: previous, play, next. */
    QRectF mediaButtons[3];

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    Locker *m_locker;
    QPointer<QScreen> m_screen;
};

/* Watches the root's children for anything mapped or raised over us. */
class RootWatcher : public QAbstractNativeEventFilter {
public:
    std::function<void(xcb_window_t)> changed;

    bool nativeEventFilter(const QByteArray &type, void *message,
                           qintptr *) override
    {
        if (type != "xcb_generic_event_t")
            return false;
        const auto *ev = static_cast<const xcb_generic_event_t *>(message);
        switch (ev->response_type & 0x7f) {
        case XCB_MAP_NOTIFY:
            changed(reinterpret_cast<const xcb_map_notify_event_t *>(ev)->window);
            break;
        case XCB_CONFIGURE_NOTIFY:
            changed(reinterpret_cast<const xcb_configure_notify_event_t *>(ev)->window);
            break;
        case XCB_CIRCULATE_NOTIFY:
            changed(reinterpret_cast<const xcb_circulate_notify_event_t *>(ev)->window);
            break;
        default:
            break;
        }
        return false;
    }
};

class Locker : public QObject {
public:
    explicit Locker(int statusFd) : m_statusFd(statusFd)
    {
        mlock(m_password, sizeof(m_password));
        if (const passwd *pw = getpwuid(getuid())) {
            m_user = QByteArray(pw->pw_name);
            const QString gecos = QString::fromLocal8Bit(pw->pw_gecos ? pw->pw_gecos : "")
                                      .section(QLatin1Char(','), 0, 0)
                                      .trimmed();
            m_fullName = gecos.isEmpty() ? QString::fromLocal8Bit(m_user) : gecos;
        }
        m_service = pamService();
        m_avatar = avatarImage(m_user);
        m_wallpaper = wallpaperImage();

        const QHash<QString, QString> conf = Paths::readConf(QStringLiteral("lock.conf"));
        m_displayOffSecs = conf.value(QStringLiteral("display_off"),
                                      QStringLiteral("300")).toInt();

        m_anim.setDuration(220);
        m_anim.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_anim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) {
                    m_t = v.toDouble();
                    updatePrimary();
                });
        m_shake.setDuration(360);
        m_shake.setStartValue(0.0);
        m_shake.setEndValue(1.0);
        connect(&m_shake, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) {
                    const double t = v.toDouble();
                    m_shakeX = std::sin(t * M_PI * 6) * 10 * (1 - t);
                    updatePrimary();
                });

        m_clock.setSingleShot(true);
        connect(&m_clock, &QTimer::timeout, this, [this]() {
            m_uptime = uptimeText();
            updateAll();
            scheduleClock();
        });
        m_promptIdle.setSingleShot(true);
        m_promptIdle.setInterval(promptIdleMs);
        connect(&m_promptIdle, &QTimer::timeout, this, [this]() {
            if (!m_checking)
                showPrompt(false);
        });
        m_cursorIdle.setSingleShot(true);
        m_cursorIdle.setInterval(cursorIdleMs);
        connect(&m_cursorIdle, &QTimer::timeout, this, [this]() {
            for (LockWindow *w : m_windows)
                w->setCursor(Qt::BlankCursor);
        });
        m_displayOff.setSingleShot(true);
        connect(&m_displayOff, &QTimer::timeout, this, &Locker::displaysOff);
        m_wake.setInterval(1000);
        connect(&m_wake, &QTimer::timeout, this, &Locker::checkWake);
        m_restack.setSingleShot(true);
        connect(&m_restack, &QTimer::timeout, this, &Locker::restack);
        /* A belt to the root watcher's braces, and the grabs re-asserted in
         * case anything took them. */
        m_guard.setInterval(2000);
        connect(&m_guard, &QTimer::timeout, this, [this]() {
            restack();
            if (m_primary) {
                m_primary->windowHandle()->setKeyboardGrabEnabled(true);
                m_primary->windowHandle()->setMouseGrabEnabled(true);
            }
        });

        m_weather.setCommand({QStringLiteral("fanshell-qs-weather")});
        connect(&m_weather, &CollectorProcess::finished, this,
                [this](const QString &text) {
                    m_weatherIcon.clear();
                    m_weatherText.clear();
                    QString temp, cond;
                    for (const QString &line : text.trimmed().split(QLatin1Char('\n'))) {
                        const int i = line.indexOf(QLatin1Char('='));
                        if (i < 0)
                            continue;
                        const QString k = line.left(i), v = line.mid(i + 1);
                        if (k == QLatin1String("icon"))
                            m_weatherIcon = v;
                        else if (k == QLatin1String("temp"))
                            temp = v;
                        else if (k == QLatin1String("cond"))
                            cond = v;
                    }
                    if (!temp.isEmpty())
                        m_weatherText = cond.isEmpty() ? temp : temp + QStringLiteral("  ") + cond;
                    updatePrimary();
                });
        auto *weatherTimer = new QTimer(this);
        connect(weatherTimer, &QTimer::timeout, &m_weather,
                [this]() { m_weather.start(); });
        weatherTimer->start(15 * 60 * 1000);
        m_weather.start();

        connect(BatteryState::instance(), &BatteryState::changed, this,
                [this]() { updatePrimary(); });
        connect(MediaControls::instance(), &MediaControls::changed, this,
                [this]() { mediaChanged(); });
        mediaChanged();

        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
            connect(s, &QScreen::geometryChanged, this, &Locker::syncWindows);
            syncWindows();
        });
        connect(qApp, &QGuiApplication::screenRemoved, this,
                [this]() { QTimer::singleShot(0, this, &Locker::syncWindows); });
        connect(qApp, &QGuiApplication::primaryScreenChanged, this,
                &Locker::syncWindows);
        for (QScreen *s : QGuiApplication::screens())
            connect(s, &QScreen::geometryChanged, this, &Locker::syncWindows);
    }

    ~Locker() override { clearPassword(); }

    /* Cover every output and take the keyboard and pointer. */
    bool lock()
    {
        m_uptime = uptimeText();
        syncWindows();
        if (!m_primary || !grab(m_primary, 60))
            return false;
        m_locked = true;

        xcb_connection_t *c = conn();
        const xcb_window_t root = rootWindow(c);
        /* Event masks are per client: OR ours into what Qt selected. */
        uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        if (auto *a = xcb_get_window_attributes_reply(
                c, xcb_get_window_attributes(c, root), nullptr)) {
            mask |= a->your_event_mask;
            free(a);
        }
        xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &mask);
        xcb_flush(c);
        m_watcher.changed = [this](xcb_window_t w) {
            if (!ours(w) && !m_restack.isActive())
                m_restack.start(0);
        };
        qApp->installNativeEventFilter(&m_watcher);
        restack();
        m_guard.start();

        if (m_statusFd >= 0) {
            const char locked = 'L';
            if (write(m_statusFd, &locked, 1) < 0)
                perror("fanshell-qs-shell lock");
        }
        notifyShell(this, QStringLiteral("lock"), QStringLiteral("locked"));
        scheduleClock();
        activity();
        return true;
    }

    void paint(LockWindow *w, QPainter &p);
    void key(QKeyEvent *e);
    void press(LockWindow *w, const QPointF &pos);

    void activity()
    {
        if (m_displayOffSecs > 0)
            m_displayOff.start(m_displayOffSecs * 1000);
        if (m_prompt)
            m_promptIdle.start();
    }

    void pointerMoved()
    {
        activity();
        if (!m_cursorIdle.isActive())
            for (LockWindow *w : m_windows)
                w->setCursor(Qt::ArrowCursor);
        m_cursorIdle.start();
    }

private:
    bool ours(xcb_window_t id) const
    {
        for (LockWindow *w : m_windows)
            if (xcb_window_t(w->winId()) == id)
                return true;
        return false;
    }

    /* One window per distinct output rectangle (mirrored outputs share one,
     * or the two would keep raising over each other). */
    void syncWindows()
    {
        QVector<LockWindow *> keep;
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (QScreen *s : screens) {
            const QRect g = s->geometry();
            bool covered = false;
            for (LockWindow *w : keep)
                covered = covered || w->geometry() == g;
            if (covered)
                continue;
            LockWindow *win = nullptr;
            for (LockWindow *w : m_windows)
                if (w->lockScreen() == s && !keep.contains(w))
                    win = w;
            if (!win) {
                win = new LockWindow(this, s);
                win->show();
            } else if (win->geometry() != g) {
                win->setGeometry(g);
            }
            keep << win;
        }
        for (LockWindow *w : m_windows)
            if (!keep.contains(w))
                w->deleteLater();
        m_windows = keep;

        LockWindow *primary = keep.isEmpty() ? nullptr : keep.first();
        if (QScreen *ps = QGuiApplication::primaryScreen())
            for (LockWindow *w : keep)
                if (w->geometry() == ps->geometry())
                    primary = w;
        if (primary != m_primary) {
            m_primary = primary;
            /* The grabs move to the window that takes the keyboard now. */
            if (m_locked && m_primary)
                grab(m_primary, 20);
        }
        if (m_locked)
            restack();
        updateAll();
    }

    bool grab(LockWindow *w, int attempts)
    {
        bool keyboard = false, pointer = false;
        for (int i = 0; i < attempts && !(keyboard && pointer); i++) {
            /* Until the map has gone through, X refuses: not viewable. */
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (!w->windowHandle())
                continue;
            keyboard = keyboard || w->windowHandle()->setKeyboardGrabEnabled(true);
            pointer = pointer || w->windowHandle()->setMouseGrabEnabled(true);
            if (!(keyboard && pointer))
                usleep(50000);
        }
        if (!keyboard || !pointer)
            fprintf(stderr, "fanshell-qs-shell lock: cannot grab the %s\n",
                    keyboard ? "pointer" : "keyboard");
        return keyboard && pointer;
    }

    /* Our windows back on top of anything mapped above them. */
    void restack()
    {
        xcb_connection_t *c = conn();
        if (!c || m_windows.isEmpty())
            return;
        xcb_query_tree_reply_t *tree =
            xcb_query_tree_reply(c, xcb_query_tree(c, rootWindow(c)), nullptr);
        if (!tree)
            return;
        const xcb_window_t *kids = xcb_query_tree_children(tree);
        const int n = xcb_query_tree_children_length(tree);
        QVector<xcb_get_window_attributes_cookie_t> cookies(n);
        for (int i = 0; i < n; i++)
            cookies[i] = xcb_get_window_attributes(c, kids[i]);
        int lowestOurs = n, topForeign = -1;
        for (int i = 0; i < n; i++) {
            xcb_get_window_attributes_reply_t *a =
                xcb_get_window_attributes_reply(c, cookies[i], nullptr);
            const bool viewable = a && a->map_state == XCB_MAP_STATE_VIEWABLE;
            free(a);
            if (ours(kids[i]))
                lowestOurs = qMin(lowestOurs, i);
            else if (viewable)
                topForeign = i;
        }
        free(tree);
        if (topForeign < lowestOurs)
            return;
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        for (LockWindow *w : m_windows)
            if (w != m_primary)
                xcb_configure_window(c, xcb_window_t(w->winId()),
                                     XCB_CONFIG_WINDOW_STACK_MODE, &above);
        if (m_primary)
            xcb_configure_window(c, xcb_window_t(m_primary->winId()),
                                 XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_flush(c);
    }

    const QPixmap &background(const QSize &size)
    {
        const quint64 key = (quint64(quint32(size.width())) << 32) | quint32(size.height());
        auto it = m_backgrounds.find(key);
        if (it == m_backgrounds.end())
            it = m_backgrounds.insert(key, blurredBackground(m_wallpaper, size));
        return *it;
    }

    void scheduleClock()
    {
        const QTime now = QTime::currentTime();
        m_clock.start((60 - now.second()) * 1000 - now.msec() + 20);
    }

    void updatePrimary()
    {
        if (m_primary)
            m_primary->update();
    }

    void updateAll()
    {
        for (LockWindow *w : m_windows)
            w->update();
    }

    void showPrompt(bool on)
    {
        if (on == m_prompt)
            return;
        m_prompt = on;
        if (!on) {
            clearPassword();
            m_wrong = false;
            m_promptIdle.stop();
        } else {
            m_promptIdle.start();
        }
        m_anim.stop();
        m_anim.setStartValue(m_t);
        m_anim.setEndValue(on ? 1.0 : 0.0);
        m_anim.start();
    }

    void clearPassword()
    {
        explicit_bzero(m_password, sizeof(m_password));
        m_length = 0;
        m_glyphs = 0;
    }

    void append(const QString &text)
    {
        const QByteArray utf8 = text.toUtf8();
        if (m_length + utf8.size() >= int(sizeof(m_password)))
            return;
        memcpy(m_password + m_length, utf8.constData(), size_t(utf8.size()));
        m_length += int(utf8.size());
        m_glyphs++;
    }

    /* Drop the last character: its UTF-8 continuation bytes, then the lead. */
    void backspace()
    {
        while (m_length > 0 && (uchar(m_password[m_length - 1]) & 0xc0) == 0x80)
            m_password[--m_length] = 0;
        if (m_length > 0)
            m_password[--m_length] = 0;
        m_glyphs = qMax(0, m_glyphs - 1);
    }

    void submit()
    {
        if (m_checking || m_length == 0)
            return;
        m_checking = true;
        m_wrong = false;
        m_promptIdle.stop();
        updatePrimary();
        /* The thread gets its own copy and wipes it; ours goes now. */
        char *copy = static_cast<char *>(malloc(size_t(m_length) + 1));
        memcpy(copy, m_password, size_t(m_length));
        copy[m_length] = 0;
        clearPassword();
        const QByteArray service = m_service, user = m_user;
        QPointer<Locker> self(this);
        std::thread([self, service, user, copy]() {
            const size_t len = strlen(copy);
            const bool ok = authenticate(service, user, copy);
            explicit_bzero(copy, len);
            free(copy);
            QMetaObject::invokeMethod(qApp, [self, ok]() {
                if (self)
                    self->checked(ok);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void checked(bool ok)
    {
        m_checking = false;
        if (ok) {
            unlock();
            return;
        }
        m_failures++;
        m_wrong = true;
        m_shake.stop();
        m_shake.start();
        m_promptIdle.start();
        updatePrimary();
    }

    void unlock()
    {
        m_locked = false;
        m_guard.stop();
        restoreDpms();
        if (m_primary) {
            m_primary->windowHandle()->setKeyboardGrabEnabled(false);
            m_primary->windowHandle()->setMouseGrabEnabled(false);
        }
        for (LockWindow *w : m_windows)
            w->hide();
        notifyShell(this, QStringLiteral("lock"), QStringLiteral("unlocked"),
                    []() { QCoreApplication::exit(lockerUnlocked); });
    }

    void displaysOff()
    {
        xcb_connection_t *c = conn();
        const xcb_query_extension_reply_t *ext =
            c ? xcb_get_extension_data(c, &xcb_dpms_id) : nullptr;
        if (!ext || !ext->present)
            return;
        /* Forcing a level needs DPMS on; on for now, off again on waking. */
        if (auto *info = xcb_dpms_info_reply(c, xcb_dpms_info(c), nullptr)) {
            if (!info->state) {
                xcb_dpms_enable(c);
                m_enabledDpms = true;
            }
            free(info);
        }
        xcb_dpms_force_level(c, XCB_DPMS_DPMS_MODE_OFF);
        xcb_flush(c);
        m_wake.start();
    }

    void checkWake()
    {
        xcb_connection_t *c = conn();
        if (!c)
            return;
        xcb_dpms_info_reply_t *info = xcb_dpms_info_reply(c, xcb_dpms_info(c), nullptr);
        const bool awake = !info || info->power_level == XCB_DPMS_DPMS_MODE_ON;
        free(info);
        if (!awake)
            return;
        m_wake.stop();
        restoreDpms();
        activity();
    }

    void restoreDpms()
    {
        if (!m_enabledDpms)
            return;
        m_enabledDpms = false;
        if (xcb_connection_t *c = conn()) {
            xcb_dpms_disable(c);
            xcb_flush(c);
        }
    }

    bool capsLock() const
    {
        xcb_connection_t *c = conn();
        if (!c)
            return false;
        xcb_xkb_get_state_reply_t *s = xcb_xkb_get_state_reply(
            c, xcb_xkb_get_state(c, XCB_XKB_ID_USE_CORE_KBD), nullptr);
        const bool on = s && (s->lockedMods & XCB_MOD_MASK_LOCK);
        free(s);
        return on;
    }

    void mediaChanged()
    {
        m_media = MediaControls::instance()->nowPlaying();
        if (m_media.artUrl != m_artUrl) {
            m_artUrl = m_media.artUrl;
            m_art = QPixmap();
            const QUrl url(m_artUrl);
            if (url.isLocalFile()) {
                m_art.load(url.toLocalFile());
            } else if (url.scheme() == QLatin1String("https")
                       || url.scheme() == QLatin1String("http")) {
                if (!m_nam)
                    m_nam = new QNetworkAccessManager(this);
                QNetworkRequest req(url);
                req.setTransferTimeout(5000);
                QNetworkReply *reply = m_nam->get(req);
                const QString wanted = m_artUrl;
                connect(reply, &QNetworkReply::finished, this, [this, reply, wanted]() {
                    reply->deleteLater();
                    if (wanted == m_artUrl && reply->error() == QNetworkReply::NoError) {
                        m_art.loadFromData(reply->readAll());
                        updatePrimary();
                    }
                });
            }
        }
        updatePrimary();
    }

    void drawText(QPainter &p, const QRectF &r, int flags, const QString &text,
                  const QColor &color)
    {
        p.setPen(QColor(0, 0, 0, 80));
        p.drawText(r.translated(0, 2), flags, text);
        p.setPen(color);
        p.drawText(r, flags, text);
    }

    void paintMedia(LockWindow *w, QPainter &p, const QRectF &card);
    void paintPrompt(QPainter &p, const QRectF &r, double top);

    int m_statusFd;
    QByteArray m_user;
    QString m_fullName;
    QByteArray m_service;
    QImage m_avatar;
    QImage m_wallpaper;
    QHash<quint64, QPixmap> m_backgrounds;

    QVector<LockWindow *> m_windows;
    LockWindow *m_primary = nullptr;
    RootWatcher m_watcher;
    bool m_locked = false;

    char m_password[512] = {};
    int m_length = 0;
    int m_glyphs = 0;
    bool m_checking = false;
    bool m_wrong = false;
    int m_failures = 0;

    bool m_prompt = false;
    double m_t = 0;
    QVariantAnimation m_anim;
    QVariantAnimation m_shake;
    double m_shakeX = 0;

    QTimer m_clock;
    QTimer m_promptIdle;
    QTimer m_cursorIdle;
    QTimer m_displayOff;
    QTimer m_wake;
    QTimer m_restack;
    QTimer m_guard;
    int m_displayOffSecs = 300;
    bool m_enabledDpms = false;

    CollectorProcess m_weather;
    QString m_weatherIcon, m_weatherText;
    QString m_uptime;

    MediaControls::NowPlaying m_media;
    QString m_artUrl;
    QPixmap m_art;
    QNetworkAccessManager *m_nam = nullptr;
};

void Locker::paint(LockWindow *w, QPainter &p)
{
    const QRectF r = w->rect();
    p.drawPixmap(0, 0, background(w->size()));
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const bool primary = w == m_primary;
    const double t = primary ? m_t : 0;
    if (t > 0)
        p.fillRect(r, QColor(16, 17, 26, int(70 * t)));
    const double W = r.width(), H = r.height();

    /* The clock: large and central, then small at the top for the prompt. */
    const int bigPx = qBound(80, int(H * 0.15), 168);
    const int smallPx = qBound(44, int(H * 0.06), 72);
    const int timePx = int(lerp(bigPx, smallPx, t));
    const double timeY = lerp(H * 0.34, H * 0.12, t);
    p.setFont(uiFont(timePx, QFont::Medium));
    drawText(p, QRectF(0, timeY - timePx * 0.65, W, timePx * 1.3), Qt::AlignCenter,
             QTime::currentTime().toString(QStringLiteral("HH:mm")), Theme::textStrong);
    const int datePx = int(lerp(qBound(18, int(H * 0.026), 30), 18, t));
    const double dateY = timeY + timePx * 0.58;
    p.setFont(uiFont(datePx));
    drawText(p, QRectF(0, dateY, W, datePx * 1.5), Qt::AlignCenter,
             QLocale().toString(QDate::currentDate(), QStringLiteral("dddd, d MMMM")),
             Theme::text);

    if (!primary)
        return;

    const double avatarY = H * 0.40;
    if (t > 0) {
        p.save();
        p.setOpacity(t);
        paintPrompt(p, r, avatarY);
        p.restore();
    }
    if (t < 1) {
        p.save();
        p.setOpacity(1 - t);
        p.setFont(uiFont(14));
        drawText(p, QRectF(0, H - 64, W, 24), Qt::AlignCenter,
                 QStringLiteral("Press a key or click to unlock"), Theme::brightwhite);
        p.restore();
    }

    for (QRectF &b : w->mediaButtons)
        b = QRectF();
    if (!m_media.title.isEmpty()) {
        const double cardW = qMin(440.0, W - 48), cardH = 88;
        const double top = lerp(dateY + datePx * 1.5 + 40, H * 0.72, t);
        paintMedia(w, p, QRectF((W - cardW) / 2, top, cardW, cardH));
    }

    /* Weather bottom left, uptime bottom right, battery top right. */
    const double edge = 24;
    if (!m_weatherText.isEmpty()) {
        double x = edge;
        if (!m_weatherIcon.isEmpty()) {
            p.setFont(Theme::font(20));
            drawText(p, QRectF(x, H - edge - 28, 28, 28), Qt::AlignCenter, m_weatherIcon,
                     Theme::text);
            x += 34;
        }
        p.setFont(uiFont(16));
        drawText(p, QRectF(x, H - edge - 28, W / 2 - x, 28),
                 Qt::AlignLeft | Qt::AlignVCenter, m_weatherText, Theme::text);
    }
    if (!m_uptime.isEmpty()) {
        p.setFont(uiFont(16));
        drawText(p, QRectF(W / 2, H - edge - 28, W / 2 - edge, 28),
                 Qt::AlignRight | Qt::AlignVCenter, m_uptime, Theme::text);
    }
    const BatteryState *battery = BatteryState::instance();
    if (battery->present) {
        const QString pct = QStringLiteral("%1%").arg(battery->percent);
        p.setFont(uiFont(16, QFont::DemiBold));
        const double pw = QFontMetricsF(p.font()).horizontalAdvance(pct);
        drawText(p, QRectF(W - edge - pw, edge, pw, 28), Qt::AlignRight | Qt::AlignVCenter,
                 pct, Theme::text);
        p.setFont(Theme::font(20));
        drawText(p, QRectF(W - edge - pw - 34, edge, 28, 28), Qt::AlignCenter, battery->icon,
                 battery->color.isValid() ? battery->color : Theme::text);
    }
}

void Locker::paintPrompt(QPainter &p, const QRectF &r, double avatarY)
{
    const double W = r.width();
    const double radius = 52;
    const QRectF circle(W / 2 - radius, avatarY - radius, 2 * radius, 2 * radius);
    if (!m_avatar.isNull()) {
        QPainterPath clip;
        clip.addEllipse(circle);
        p.save();
        p.setClipPath(clip);
        const QImage img = m_avatar.scaled(circle.size().toSize(),
                                           Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
        p.drawImage(QPointF(circle.center().x() - img.width() / 2.0,
                            circle.center().y() - img.height() / 2.0), img);
        p.restore();
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::surfaceHover);
        p.drawEllipse(circle);
        p.setFont(uiFont(44, QFont::Medium));
        p.setPen(Theme::textStrong);
        p.drawText(circle, Qt::AlignCenter, m_fullName.left(1).toUpper());
    }
    p.setPen(QPen(QColor(255, 255, 255, 40), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(circle);

    p.setFont(uiFont(22, QFont::DemiBold));
    drawText(p, QRectF(0, circle.bottom() + 14, W, 32), Qt::AlignCenter, m_fullName,
             Theme::textStrong);

    /* The password field. */
    const double fw = qMin(360.0, W - 48), fh = 46;
    const QRectF field((W - fw) / 2 + m_shakeX, circle.bottom() + 62, fw, fh);
    const QColor edge = m_checking ? Theme::cyan : m_wrong ? Theme::red : Theme::blue;
    Theme::paintRect(p, field, QColor(26, 27, 38, 200), fh / 2, edge, 2);

    const QRectF arrow(field.right() - fh, field.top(), fh, fh);
    p.setFont(Theme::font(20));
    p.setPen(m_length > 0 ? Theme::textStrong : Theme::textMuted);
    p.drawText(arrow, Qt::AlignCenter, glyph(U'\U000F0054'));

    const QRectF inner = field.adjusted(22, 0, -fh, 0);
    if (m_checking) {
        p.setFont(uiFont(15));
        p.setPen(Theme::textMuted);
        p.drawText(inner, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Checking…"));
    } else if (m_glyphs == 0) {
        p.setFont(uiFont(15));
        p.setPen(Theme::textMuted);
        p.drawText(inner, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Password"));
    } else {
        const double step = 16, dot = 9;
        const int fits = qMax(1, int(inner.width() / step));
        const int shown = qMin(m_glyphs, fits);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::textStrong);
        for (int i = 0; i < shown; i++)
            p.drawEllipse(QRectF(inner.left() + i * step, field.center().y() - dot / 2,
                                 dot, dot));
    }

    QString message;
    QColor color;
    if (m_wrong) {
        message = m_failures > 1
                      ? QStringLiteral("Wrong password · %1 attempts").arg(m_failures)
                      : QStringLiteral("Wrong password");
        color = Theme::red;
    } else if (capsLock()) {
        message = QStringLiteral("Caps Lock is on");
        color = Theme::yellow;
    }
    if (!message.isEmpty()) {
        p.setFont(uiFont(14));
        drawText(p, QRectF(0, field.bottom() + 12, W, 22), Qt::AlignCenter, message, color);
    }
}

void Locker::paintMedia(LockWindow *w, QPainter &p, const QRectF &card)
{
    Theme::paintRect(p, card, QColor(26, 27, 38, 170), 14, QColor(255, 255, 255, 28), 1);
    const double pad = 12, art = card.height() - 2 * pad;
    const QRectF artRect(card.left() + pad, card.top() + pad, art, art);
    if (!m_art.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(artRect, 8, 8);
        p.save();
        p.setClipPath(clip);
        const QPixmap scaled = m_art.scaled(artRect.size().toSize(),
                                            Qt::KeepAspectRatioByExpanding,
                                            Qt::SmoothTransformation);
        p.drawPixmap(QPointF(artRect.center().x() - scaled.width() / 2.0,
                             artRect.center().y() - scaled.height() / 2.0), scaled);
        p.restore();
    } else {
        Theme::paintRect(p, artRect, Theme::surface, 8);
        p.setFont(Theme::font(28));
        p.setPen(Theme::textMuted);
        p.drawText(artRect, Qt::AlignCenter, glyph(U'\U000F075A'));
    }

    const double button = 36, gap = 6;
    const double buttonsW = 3 * button + 2 * gap;
    const double textX = artRect.right() + 14;
    const double textW = card.right() - pad - buttonsW - 10 - textX;
    const QFont title = uiFont(15, QFont::DemiBold), artist = uiFont(13);
    p.setFont(title);
    p.setPen(Theme::textStrong);
    p.drawText(QRectF(textX, card.top() + 18, textW, 24), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetricsF(title).elidedText(m_media.title, Qt::ElideRight, textW));
    p.setFont(artist);
    p.setPen(Theme::brightwhite);
    p.drawText(QRectF(textX, card.top() + 44, textW, 22), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetricsF(artist).elidedText(m_media.artist, Qt::ElideRight, textW));

    const char32_t glyphs[3] = {U'\U000F04AE',
                                m_media.playing ? U'\U000F03E4' : U'\U000F040A',
                                U'\U000F04AD'};
    const bool enabled[3] = {m_media.canPrevious, m_media.canPlayPause, m_media.canNext};
    double x = card.right() - pad - buttonsW;
    for (int i = 0; i < 3; i++) {
        const QRectF b(x, card.center().y() - button / 2, button, button);
        if (i == 1)
            Theme::paintRect(p, b, QColor(255, 255, 255, 30), button / 2);
        p.setFont(Theme::font(20));
        p.setPen(enabled[i] ? Theme::textStrong : Theme::textMuted);
        p.drawText(b, Qt::AlignCenter, glyph(glyphs[i]));
        w->mediaButtons[i] = enabled[i] ? b : QRectF();
        x += button + gap;
    }
}

void Locker::key(QKeyEvent *e)
{
    activity();
    switch (e->key()) {
    /* Hardware keys work as they would unlocked; they do not wake the
     * prompt. */
    case Qt::Key_VolumeUp:
        notifyShell(this, QStringLiteral("audio"), QStringLiteral("volume-up"));
        return;
    case Qt::Key_VolumeDown:
        notifyShell(this, QStringLiteral("audio"), QStringLiteral("volume-down"));
        return;
    case Qt::Key_VolumeMute:
        notifyShell(this, QStringLiteral("audio"), QStringLiteral("toggle-mute"));
        return;
    case Qt::Key_MicMute:
        notifyShell(this, QStringLiteral("audio"), QStringLiteral("toggle-mic"));
        return;
    case Qt::Key_MonBrightnessUp:
        notifyShell(this, QStringLiteral("brightness"), QStringLiteral("up"));
        return;
    case Qt::Key_MonBrightnessDown:
        notifyShell(this, QStringLiteral("brightness"), QStringLiteral("down"));
        return;
    case Qt::Key_KeyboardBrightnessUp:
        notifyShell(this, QStringLiteral("brightness"), QStringLiteral("keyboard-up"));
        return;
    case Qt::Key_KeyboardBrightnessDown:
        notifyShell(this, QStringLiteral("brightness"), QStringLiteral("keyboard-down"));
        return;
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaPause:
    case Qt::Key_MediaTogglePlayPause:
        MediaControls::instance()->playPause();
        return;
    case Qt::Key_MediaNext:
        MediaControls::instance()->next();
        return;
    case Qt::Key_MediaPrevious:
        MediaControls::instance()->previous();
        return;
    default:
        break;
    }
    if (m_checking)
        return;

    const QString text = e->text();
    const bool printable = !text.isEmpty() && text.at(0).isPrint()
                           && !(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
    if (!m_prompt) {
        if (e->key() == Qt::Key_Escape)
            return;
        showPrompt(true);
        if (printable)
            append(text);
        updatePrimary();
        return;
    }

    m_wrong = false;
    switch (e->key()) {
    case Qt::Key_Escape:
        if (m_length > 0)
            clearPassword();
        else
            showPrompt(false);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        submit();
        break;
    case Qt::Key_Backspace:
        if (e->modifiers() & Qt::ControlModifier)
            clearPassword();
        else
            backspace();
        break;
    case Qt::Key_U:
        if (e->modifiers() & Qt::ControlModifier) {
            clearPassword();
            break;
        }
        Q_FALLTHROUGH();
    default:
        if (printable)
            append(text);
        break;
    }
    updatePrimary();
}

void Locker::press(LockWindow *w, const QPointF &pos)
{
    activity();
    if (w == m_primary) {
        if (w->mediaButtons[0].contains(pos)) {
            MediaControls::instance()->previous();
            return;
        }
        if (w->mediaButtons[1].contains(pos)) {
            MediaControls::instance()->playPause();
            return;
        }
        if (w->mediaButtons[2].contains(pos)) {
            MediaControls::instance()->next();
            return;
        }
    }
    if (!m_prompt)
        showPrompt(true);
    else if (!m_checking && m_length > 0) {
        /* The arrow at the end of the field submits. */
        const double W = m_primary->width(), H = m_primary->height();
        const double fw = qMin(360.0, W - 48);
        const QRectF arrow((W + fw) / 2 - 46, H * 0.40 + 52 + 62, 46, 46);
        if (w == m_primary && arrow.contains(pos))
            submit();
    }
}

void LockWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    m_locker->paint(this, p);
}

void LockWindow::keyPressEvent(QKeyEvent *e)
{
    m_locker->key(e);
}

void LockWindow::mousePressEvent(QMouseEvent *e)
{
    m_locker->press(this, e->position());
}

void LockWindow::mouseMoveEvent(QMouseEvent *)
{
    m_locker->pointerMoved();
}

void LockWindow::wheelEvent(QWheelEvent *)
{
    m_locker->pointerMoved();
}

/* ------------------------------------------------------------ processes */

volatile sig_atomic_t g_stop = 0;

void onStop(int)
{
    g_stop = 1;
}

/* The locker process: Qt, the windows, the grabs. */
int runLocker(int argc, char **argv, int statusFd)
{
    prctl(PR_SET_NAME, processName);
    /* No core dumps or ptrace by the user's other programs: the password
     * passes through this process's memory. */
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    qputenv("QT_QPA_PLATFORM", "xcb");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fanshell-qs-lock"));
    app.setQuitOnLastWindowClosed(false);
    if (!Surface::active())
        return lockerGrabFailed;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &ProcUtil::terminateAll);
    /* Media keys and buttons act here, but nothing is shown over the lock. */
    HardwareOsd::setSuppressed(true);

    Locker locker(statusFd);
    if (!locker.lock())
        return lockerGrabFailed;
    return app.exec();
}

/* The supervisor: keeps a locker running until one exits unlocked. */
int supervise(int argc, char **argv, int notifyFd)
{
    prctl(PR_SET_NAME, processName);
    struct sigaction sa = {};
    sa.sa_handler = onStop;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: read() and waitpid() return EINTR to act on it. */
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
    /* Nothing reads the caller's stdout any more; stderr stays, for a
     * terminal, and a closed one costs nothing now SIGPIPE is ignored. */
    if (int null = open("/dev/null", O_RDWR); null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        close(null);
    }

    bool everLocked = false;
    int quickFailures = 0;
    for (;;) {
        int status[2];
        if (pipe2(status, O_CLOEXEC) < 0)
            return 1;
        timespec started;
        clock_gettime(CLOCK_MONOTONIC, &started);
        const pid_t pid = fork();
        if (pid < 0) {
            close(status[0]);
            close(status[1]);
            if (!everLocked)
                return 1;
            sleep(1);
            continue;
        }
        if (pid == 0) {
            close(status[0]);
            if (notifyFd >= 0)
                close(notifyFd);
            _exit(runLocker(argc, argv, status[1]));
        }
        close(status[1]);

        char byte = 0;
        ssize_t n;
        while ((n = read(status[0], &byte, 1)) < 0 && errno == EINTR && !g_stop) {
        }
        close(status[0]);
        if (n == 1 && !everLocked) {
            everLocked = true;
            if (notifyFd >= 0) {
                if (write(notifyFd, &byte, 1) < 0) {
                }
                close(notifyFd);
                notifyFd = -1;
            }
        }
        int wstatus = 0;
        if (g_stop)
            kill(pid, SIGTERM);
        while (waitpid(pid, &wstatus, 0) < 0) {
            if (errno != EINTR)
                break;
            if (g_stop)
                kill(pid, SIGTERM);
        }
        if (g_stop)
            return 0;
        if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == lockerUnlocked)
            return 0;
        /* The X session has ended: nothing left to lock. */
        if (!displayAlive())
            return 0;
        if (!everLocked)
            return 1;

        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - started.tv_sec < 2) {
            /* Dying as fast as it starts: keep trying, but not flat out. */
            quickFailures++;
            usleep(useconds_t(qMin(250 * quickFailures, 2000)) * 1000);
        } else {
            quickFailures = 0;
        }
        fprintf(stderr, "fanshell-qs-shell lock: the lock screen died (%s %d); "
                        "starting it again\n",
                WIFSIGNALED(wstatus) ? "signal" : "status",
                WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : WEXITSTATUS(wstatus));
    }
}

QString displayKey()
{
    QString key = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("DISPLAY"), QStringLiteral(":0"));
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char(':'), QLatin1Char('x'));
    return key;
}

} /* namespace */

namespace LockScreen {

QString lockFilePath()
{
    QString dir = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("XDG_RUNTIME_DIR"));
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return dir + QStringLiteral("/fanshell-qs-lock-") + displayKey()
           + QStringLiteral(".lock");
}

bool isLocked()
{
    const int fd = open(QFile::encodeName(lockFilePath()).constData(),
                        O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool held = flock(fd, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK;
    close(fd);
    return held;
}

int run(int argc, char **argv)
{
    const char *display = getenv("DISPLAY");
    if (!display || !*display || !displayAlive()) {
        fprintf(stderr, "fanshell-qs-shell lock: no X display\n");
        return 2;
    }
    if (pamService().isEmpty()) {
        fprintf(stderr, "fanshell-qs-shell lock: no PAM service to check the "
                        "password with; install /etc/pam.d/fanshell-qs-lock "
                        "(tools/install-shell does)\n");
        return 2;
    }

    const int fd = open(QFile::encodeName(lockFilePath()).constData(),
                        O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("fanshell-qs-shell lock");
        return 2;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        const bool held = errno == EWOULDBLOCK;
        close(fd);
        return held ? 0 : 2; /* already locked: done */
    }

    int ready[2];
    if (pipe2(ready, O_CLOEXEC) < 0)
        return 2;
    const pid_t pid = fork();
    if (pid < 0)
        return 2;
    if (pid == 0) {
        close(ready[0]);
        /* Out of the caller's session and process group, so whatever
         * started us can go without taking the lock with it. */
        setsid();
        _exit(supervise(argc, argv, ready[1]));
    }
    /* The supervisor holds the lock file now. */
    close(ready[1]);
    close(fd);

    pollfd pfd{ready[0], POLLIN, 0};
    char byte = 0;
    ssize_t n = -1;
    while (poll(&pfd, 1, 15000) < 0 && errno == EINTR) {
    }
    if (pfd.revents)
        n = read(ready[0], &byte, 1);
    close(ready[0]);
    return n == 1 && byte == 'L' ? 0 : 1;
}

} /* namespace LockScreen */
