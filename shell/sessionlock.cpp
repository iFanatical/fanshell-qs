#include "sessionlock.h"

#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "ext-session-lock-v1-client-protocol.h"
#include "wlclient.h"

struct SessionLock::Surface {
    SessionLock *owner = nullptr;
    Wl::Output *output = nullptr;
    wl_surface *surface = nullptr;
    ext_session_lock_surface_v1 *lockSurface = nullptr;
    wl_callback *frame = nullptr;
    QSize size;
    bool configured = false;
    bool dirty = true;
};

namespace {

/* ------------------------------------------------------------- listeners */

void lockSurfaceConfigure(void *data, ext_session_lock_surface_v1 *, uint32_t serial,
                          uint32_t w, uint32_t h)
{
    auto *s = static_cast<SessionLock::Surface *>(data);
    s->owner->configure(s, serial, w, h);
}

const ext_session_lock_surface_v1_listener lockSurfaceListener = {lockSurfaceConfigure};

const ext_session_lock_v1_listener lockListener = {
    &SessionLock::lockedEvent, &SessionLock::finishedEvent,
};

void frameDone(void *data, wl_callback *cb, uint32_t)
{
    wl_callback_destroy(cb);
    auto *s = static_cast<SessionLock::Surface *>(data);
    s->frame = nullptr;
    s->owner->frameDone(s);
}

const wl_callback_listener frameListener = {frameDone};

/* A buffer frees itself once the compositor is done with it. */
struct Shm {
    wl_buffer *buffer = nullptr;
    void *data = nullptr;
    size_t size = 0;
};

void bufferRelease(void *data, wl_buffer *buffer)
{
    auto *b = static_cast<Shm *>(data);
    wl_buffer_destroy(buffer);
    munmap(b->data, b->size);
    delete b;
}

const wl_buffer_listener bufferListener = {bufferRelease};

Shm *newBuffer(int w, int h)
{
    wl_shm *shm = Wl::Client::instance()->shm();
    if (!shm || w <= 0 || h <= 0)
        return nullptr;
    const int stride = w * 4;
    const size_t size = size_t(stride) * size_t(h);
    const int fd = memfd_create("fanshell-qs-lock", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, off_t(size)) < 0) {
        close(fd);
        return nullptr;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, int32_t(size));
    auto *b = new Shm;
    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    b->data = data;
    b->size = size;
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->buffer, &bufferListener, b);
    return b;
}

void kbKeymap(void *data, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size)
{
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
        static_cast<SessionLock *>(data)->keymap(fd, size);
    else
        close(fd);
}

void kbEnter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}

void kbLeave(void *data, wl_keyboard *, uint32_t, wl_surface *)
{
    static_cast<SessionLock *>(data)->stopRepeat();
}

void kbKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    static_cast<SessionLock *>(data)->key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

void kbModifiers(void *data, wl_keyboard *, uint32_t, uint32_t depressed,
                 uint32_t latched, uint32_t locked, uint32_t group)
{
    static_cast<SessionLock *>(data)->modifiers(depressed, latched, locked, group);
}

void kbRepeat(void *data, wl_keyboard *, int32_t rate, int32_t delay)
{
    static_cast<SessionLock *>(data)->repeatInfo(rate, delay);
}

const wl_keyboard_listener keyboardListener = {
    kbKeymap, kbEnter, kbLeave, kbKey, kbModifiers, kbRepeat,
};

void ptEnter(void *data, wl_pointer *, uint32_t serial, wl_surface *surface,
             wl_fixed_t x, wl_fixed_t y)
{
    static_cast<SessionLock *>(data)->pointerEnter(surface, serial, wl_fixed_to_double(x),
                                                   wl_fixed_to_double(y));
}

void ptLeave(void *, wl_pointer *, uint32_t, wl_surface *) {}

void ptMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    static_cast<SessionLock *>(data)->pointerMotion(wl_fixed_to_double(x),
                                                    wl_fixed_to_double(y));
}

void ptButton(void *data, wl_pointer *, uint32_t, uint32_t, uint32_t button,
              uint32_t state)
{
    static_cast<SessionLock *>(data)->pointerButton(button,
                                                    state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void ptAxis(void *data, wl_pointer *, uint32_t, uint32_t, wl_fixed_t)
{
    emit static_cast<SessionLock *>(data)->moved();
}

void ptFrame(void *, wl_pointer *) {}
void ptAxisSource(void *, wl_pointer *, uint32_t) {}
void ptAxisStop(void *, wl_pointer *, uint32_t, uint32_t) {}
void ptAxisDiscrete(void *, wl_pointer *, uint32_t, int32_t) {}

const wl_pointer_listener pointerListener = {
    ptEnter, ptLeave, ptMotion, ptButton, ptAxis,
    ptFrame, ptAxisSource, ptAxisStop, ptAxisDiscrete,
};

/* The keys the lock screen acts on, as Qt names them; letters and digits
 * by their own keysyms. */
int qtKey(xkb_keysym_t sym)
{
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
        return Qt::Key_A + int(sym - XKB_KEY_a);
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
        return Qt::Key_A + int(sym - XKB_KEY_A);
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)
        return Qt::Key_0 + int(sym - XKB_KEY_0);
    switch (sym) {
    case XKB_KEY_Escape: return Qt::Key_Escape;
    case XKB_KEY_Return: return Qt::Key_Return;
    case XKB_KEY_KP_Enter: return Qt::Key_Enter;
    case XKB_KEY_BackSpace: return Qt::Key_Backspace;
    case XKB_KEY_Tab: return Qt::Key_Tab;
    case XKB_KEY_space: return Qt::Key_Space;
    case XKB_KEY_XF86AudioRaiseVolume: return Qt::Key_VolumeUp;
    case XKB_KEY_XF86AudioLowerVolume: return Qt::Key_VolumeDown;
    case XKB_KEY_XF86AudioMute: return Qt::Key_VolumeMute;
    case XKB_KEY_XF86AudioMicMute: return Qt::Key_MicMute;
    case XKB_KEY_XF86MonBrightnessUp: return Qt::Key_MonBrightnessUp;
    case XKB_KEY_XF86MonBrightnessDown: return Qt::Key_MonBrightnessDown;
    case XKB_KEY_XF86KbdBrightnessUp: return Qt::Key_KeyboardBrightnessUp;
    case XKB_KEY_XF86KbdBrightnessDown: return Qt::Key_KeyboardBrightnessDown;
    case XKB_KEY_XF86AudioPlay: return Qt::Key_MediaPlay;
    case XKB_KEY_XF86AudioPause: return Qt::Key_MediaPause;
    case XKB_KEY_XF86AudioNext: return Qt::Key_MediaNext;
    case XKB_KEY_XF86AudioPrev: return Qt::Key_MediaPrevious;
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: return Qt::Key_Shift;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: return Qt::Key_Control;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: return Qt::Key_Alt;
    case XKB_KEY_Super_L: case XKB_KEY_Super_R: return Qt::Key_Meta;
    case XKB_KEY_Caps_Lock: return Qt::Key_CapsLock;
    default: return Qt::Key_unknown;
    }
}

} /* namespace */

/* ------------------------------------------------------------ SessionLock */

bool SessionLock::supported()
{
    Wl::Client *c = Wl::Client::instance();
    return c->ok() && c->lockManager() && c->compositor() && c->shm() && c->seat();
}

SessionLock::SessionLock(QObject *parent) : QObject(parent)
{
    m_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    connect(&m_repeat, &QTimer::timeout, this, [this]() {
        sendKey(m_repeatKey, true);
        m_repeat.setInterval(m_repeatRate > 0 ? 1000 / m_repeatRate : 40);
    });
    Wl::Client *c = Wl::Client::instance();
    connect(c, &Wl::Client::outputAdded, this, [this](Wl::Output *o) {
        if (m_lock)
            addSurface(o);
    });
    connect(c, &Wl::Client::outputRemoved, this, &SessionLock::removeSurface);
    connect(c, &Wl::Client::seatCapabilitiesChanged, this, &SessionLock::seatChanged);
}

SessionLock::~SessionLock()
{
    for (Surface *s : std::as_const(m_surfaces)) {
        if (s->frame)
            wl_callback_destroy(s->frame);
        ext_session_lock_surface_v1_destroy(s->lockSurface);
        wl_surface_destroy(s->surface);
        delete s;
    }
    if (m_keyboard)
        wl_keyboard_release(m_keyboard);
    if (m_pointer)
        wl_pointer_release(m_pointer);
    if (m_cursorSurface)
        wl_surface_destroy(m_cursorSurface);
    if (m_cursorTheme)
        wl_cursor_theme_destroy(m_cursorTheme);
    if (m_state)
        xkb_state_unref(m_state);
    if (m_keymap)
        xkb_keymap_unref(m_keymap);
    if (m_xkb)
        xkb_context_unref(m_xkb);
    Wl::Client::instance()->flush();
}

bool SessionLock::lock(int timeoutMs)
{
    Wl::Client *c = Wl::Client::instance();
    if (!supported() || m_lock)
        return m_locked;
    m_lock = ext_session_lock_manager_v1_lock(c->lockManager());
    ext_session_lock_v1_add_listener(m_lock, &lockListener, this);
    for (Wl::Output *o : c->outputs())
        if (o->done)
            addSurface(o);
    seatChanged();

    /* The compositor says `locked` once every output shows a lock surface,
     * so the first frames are drawn while this waits. */
    wl_display *d = c->display();
    QElapsedTimer clock;
    clock.start();
    while (!m_locked && !m_finished && clock.elapsed() < timeoutMs) {
        wl_display_flush(d);
        pollfd pfd{wl_display_get_fd(d), POLLIN, 0};
        const int r = poll(&pfd, 1, 50);
        if (r > 0) {
            if (wl_display_dispatch(d) < 0)
                break;
        } else if (r < 0 && errno != EINTR) {
            break;
        } else {
            wl_display_dispatch_pending(d);
        }
    }
    return m_locked;
}

void SessionLock::unlock()
{
    if (!m_lock)
        return;
    for (Surface *s : std::as_const(m_surfaces)) {
        if (s->frame)
            wl_callback_destroy(s->frame);
        ext_session_lock_surface_v1_destroy(s->lockSurface);
        wl_surface_destroy(s->surface);
        delete s;
    }
    m_surfaces.clear();
    if (m_locked)
        ext_session_lock_v1_unlock_and_destroy(m_lock);
    else
        ext_session_lock_v1_destroy(m_lock);
    m_lock = nullptr;
    m_locked = false;
    m_pointerOn = nullptr;
    /* Delivered before the process can exit. */
    Wl::Client::instance()->roundtrip();
}

void SessionLock::lockedEvent(void *data, ext_session_lock_v1 *)
{
    static_cast<SessionLock *>(data)->m_locked = true;
}

void SessionLock::finishedEvent(void *data, ext_session_lock_v1 *)
{
    auto *l = static_cast<SessionLock *>(data);
    const bool was = l->m_locked;
    l->m_finished = true;
    l->m_locked = false;
    if (was)
        emit l->finished();
}

void SessionLock::addSurface(Wl::Output *o)
{
    if (!m_lock || m_surfaces.contains(o))
        return;
    Wl::Client *c = Wl::Client::instance();
    auto *s = new Surface;
    s->owner = this;
    s->output = o;
    s->surface = wl_compositor_create_surface(c->compositor());
    s->lockSurface = ext_session_lock_v1_get_lock_surface(m_lock, s->surface, o->output);
    ext_session_lock_surface_v1_add_listener(s->lockSurface, &lockSurfaceListener, s);
    m_surfaces.insert(o, s);
    c->flush();
}

void SessionLock::removeSurface(Wl::Output *o)
{
    Surface *s = m_surfaces.take(o);
    if (!s)
        return;
    if (m_pointerOn == s)
        m_pointerOn = nullptr;
    if (s->frame)
        wl_callback_destroy(s->frame);
    ext_session_lock_surface_v1_destroy(s->lockSurface);
    wl_surface_destroy(s->surface);
    delete s;
    emit surfacesChanged();
}

QStringList SessionLock::outputs() const
{
    QStringList out;
    for (const Surface *s : m_surfaces)
        if (s->configured)
            out << s->output->name;
    return out;
}

QSize SessionLock::size(const QString &output) const
{
    for (const Surface *s : m_surfaces)
        if (s->output->name == output)
            return s->size;
    return QSize();
}

void SessionLock::configure(Surface *s, uint32_t serial, uint32_t w, uint32_t h)
{
    ext_session_lock_surface_v1_ack_configure(s->lockSurface, serial);
    const QSize size{int(w), int(h)};
    const bool changed = !s->configured || size != s->size;
    s->size = size;
    s->configured = true;
    if (changed)
        emit surfacesChanged();
    s->dirty = true;
    /* A configure needs a commit with a buffer of the new size, whatever
     * frame is pending. */
    draw(s);
}

void SessionLock::damage(const QString &output)
{
    for (Surface *s : std::as_const(m_surfaces)) {
        if (!output.isEmpty() && s->output->name != output)
            continue;
        s->dirty = true;
        if (!s->frame)
            draw(s);
    }
}

void SessionLock::frameDone(Surface *s)
{
    if (s->dirty)
        draw(s);
}

void SessionLock::draw(Surface *s)
{
    if (!s->configured || s->size.isEmpty())
        return;
    const int scale = qMax(1, s->output->scale);
    Shm *b = newBuffer(s->size.width() * scale, s->size.height() * scale);
    if (!b)
        return;
    {
        QImage img(static_cast<uchar *>(b->data), s->size.width() * scale,
                   s->size.height() * scale, s->size.width() * scale * 4,
                   QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(scale);
        img.fill(Qt::black);
        QPainter p(&img);
        if (paint)
            paint(s->output->name, p);
    }
    s->dirty = false;
    wl_surface_set_buffer_scale(s->surface, scale);
    wl_surface_attach(s->surface, b->buffer, 0, 0);
    wl_surface_damage_buffer(s->surface, 0, 0, INT32_MAX, INT32_MAX);
    s->frame = wl_surface_frame(s->surface);
    wl_callback_add_listener(s->frame, &frameListener, s);
    wl_surface_commit(s->surface);
    Wl::Client::instance()->flush();
}

/* ------------------------------------------------------------------ input */

void SessionLock::seatChanged()
{
    Wl::Client *c = Wl::Client::instance();
    const uint32_t caps = c->seatCapabilities();
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !m_keyboard) {
        m_keyboard = wl_seat_get_keyboard(c->seat());
        wl_keyboard_add_listener(m_keyboard, &keyboardListener, this);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && m_keyboard) {
        wl_keyboard_release(m_keyboard);
        m_keyboard = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !m_pointer) {
        m_pointer = wl_seat_get_pointer(c->seat());
        wl_pointer_add_listener(m_pointer, &pointerListener, this);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && m_pointer) {
        wl_pointer_release(m_pointer);
        m_pointer = nullptr;
        m_pointerOn = nullptr;
    }
    c->flush();
}

void SessionLock::keymap(int fd, uint32_t size)
{
    void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    xkb_keymap *km = xkb_keymap_new_from_string(m_xkb, static_cast<const char *>(map),
                                                XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return;
    if (m_state)
        xkb_state_unref(m_state);
    if (m_keymap)
        xkb_keymap_unref(m_keymap);
    m_keymap = km;
    m_state = xkb_state_new(km);
}

void SessionLock::modifiers(uint32_t depressed, uint32_t latched, uint32_t locked,
                            uint32_t group)
{
    if (m_state)
        xkb_state_update_mask(m_state, depressed, latched, locked, 0, 0, group);
}

void SessionLock::repeatInfo(int32_t rate, int32_t delay)
{
    m_repeatRate = rate;
    m_repeatDelay = delay;
}

bool SessionLock::capsLock() const
{
    return m_state
           && xkb_state_mod_name_is_active(m_state, XKB_MOD_NAME_CAPS,
                                           XKB_STATE_MODS_LOCKED) > 0;
}

void SessionLock::key(uint32_t key, bool down)
{
    if (!m_state)
        return;
    if (!down) {
        if (key == m_repeatKey)
            m_repeat.stop();
        return;
    }
    sendKey(key, false);
    if (m_repeatRate > 0 && xkb_keymap_key_repeats(m_keymap, key + 8)) {
        m_repeatKey = key;
        m_repeat.start(qMax(1, m_repeatDelay));
    } else {
        m_repeat.stop();
    }
}

void SessionLock::sendKey(uint32_t key, bool repeat)
{
    if (!m_state)
        return;
    const xkb_keycode_t code = key + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_state, code);
    char buf[64] = {};
    xkb_state_key_get_utf8(m_state, code, buf, sizeof(buf));
    Qt::KeyboardModifiers mods;
    auto active = [this](const char *name) {
        return xkb_state_mod_name_is_active(m_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    if (active(XKB_MOD_NAME_SHIFT))
        mods |= Qt::ShiftModifier;
    if (active(XKB_MOD_NAME_CTRL))
        mods |= Qt::ControlModifier;
    if (active(XKB_MOD_NAME_ALT))
        mods |= Qt::AltModifier;
    if (active(XKB_MOD_NAME_LOGO))
        mods |= Qt::MetaModifier;
    QKeyEvent e(QEvent::KeyPress, qtKey(sym), mods, QString::fromUtf8(buf), repeat);
    emit keyPressed(&e);
    explicit_bzero(buf, sizeof(buf));
}

void SessionLock::pointerEnter(wl_surface *surface, uint32_t serial, double x, double y)
{
    m_pointerOn = nullptr;
    for (Surface *s : std::as_const(m_surfaces))
        if (s->surface == surface)
            m_pointerOn = s;
    m_enterSerial = serial;
    m_pointerPos = QPointF(x, y);
    applyCursor();
}

void SessionLock::pointerMotion(double x, double y)
{
    m_pointerPos = QPointF(x, y);
    emit moved();
}

void SessionLock::pointerButton(uint32_t button, bool down)
{
    if (!down || button != BTN_LEFT || !m_pointerOn)
        return;
    emit pressed(m_pointerOn->output->name, m_pointerPos);
}

void SessionLock::setCursorVisible(bool visible)
{
    if (visible == m_cursorVisible)
        return;
    m_cursorVisible = visible;
    applyCursor();
}

/* The theme's arrow, or none while the cursor is hidden. */
void SessionLock::applyCursor()
{
    if (!m_pointer || !m_enterSerial)
        return;
    Wl::Client *c = Wl::Client::instance();
    if (!m_cursorVisible) {
        wl_pointer_set_cursor(m_pointer, m_enterSerial, nullptr, 0, 0);
        c->flush();
        return;
    }
    if (!m_cursorTheme) {
        const int size = qEnvironmentVariableIntValue("XCURSOR_SIZE");
        m_cursorTheme = wl_cursor_theme_load(
            qgetenv("XCURSOR_THEME").isEmpty() ? nullptr : qgetenv("XCURSOR_THEME").constData(),
            size > 0 ? size : 24, c->shm());
        m_cursorSurface = wl_compositor_create_surface(c->compositor());
    }
    wl_cursor *cursor = m_cursorTheme ? wl_cursor_theme_get_cursor(m_cursorTheme, "default")
                                      : nullptr;
    if (!cursor && m_cursorTheme)
        cursor = wl_cursor_theme_get_cursor(m_cursorTheme, "left_ptr");
    if (!cursor || !cursor->image_count)
        return;
    wl_cursor_image *image = cursor->images[0];
    wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    wl_surface_attach(m_cursorSurface, buffer, 0, 0);
    wl_surface_damage_buffer(m_cursorSurface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(m_cursorSurface);
    wl_pointer_set_cursor(m_pointer, m_enterSerial, m_cursorSurface,
                          int32_t(image->hotspot_x), int32_t(image->hotspot_y));
    c->flush();
}
