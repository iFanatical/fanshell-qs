#include "volume.h"

#include <QMouseEvent>
#include <QTimer>

#include <pulse/pulseaudio.h>

/* ------------------------------------------------------------ PulseBackend */

struct PulseBackend::Priv {
    pa_threaded_mainloop *loop = nullptr;
    pa_context *ctx = nullptr;
    QString defaultSinkName;
    QString defaultSourceName;
    uint8_t sinkChannels = 2;
    uint8_t sourceChannels = 1;
    /* GUI thread only: a reconnect is already scheduled. */
    bool reconnectPending = false;

    /* Pending state assembled on the PA thread, then applied on the GUI
     * thread via queued invocation. */
};

struct PulseCallbacks {
    static void contextState(pa_context *c, void *ud)
    {
        auto *self = static_cast<PulseBackend *>(ud);
        const pa_context_state_t state = pa_context_get_state(c);
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            /* The server went away (pipewire-pulse restarted, e.g. from the
             * menu's audio recovery). NOFAIL only covers the initial
             * connect; a dropped context stays dead, so build a new one --
             * from the GUI thread, never from inside this callback. */
            QMetaObject::invokeMethod(
                self, [self]() { self->connectionLost(); },
                Qt::QueuedConnection);
            return;
        }
        if (state == PA_CONTEXT_READY) {
            pa_context_set_subscribe_callback(c, subscribe, ud);
            pa_operation *o = pa_context_subscribe(
                c,
                (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK
                                         | PA_SUBSCRIPTION_MASK_SOURCE
                                         | PA_SUBSCRIPTION_MASK_SERVER),
                nullptr, nullptr);
            if (o)
                pa_operation_unref(o);
            queryAll(self);
        }
    }

    static void subscribe(pa_context *, pa_subscription_event_type_t, uint32_t,
                          void *ud)
    {
        queryAll(static_cast<PulseBackend *>(ud));
    }

    static void queryAll(PulseBackend *self)
    {
        pa_operation *o =
            pa_context_get_server_info(self->d->ctx, serverInfo, self);
        if (o)
            pa_operation_unref(o);
    }

    static void serverInfo(pa_context *c, const pa_server_info *i, void *ud)
    {
        auto *self = static_cast<PulseBackend *>(ud);
        self->d->defaultSinkName =
            QString::fromUtf8(i->default_sink_name ? i->default_sink_name : "");
        self->d->defaultSourceName = QString::fromUtf8(
            i->default_source_name ? i->default_source_name : "");
        pa_operation *o;
        if (!self->d->defaultSinkName.isEmpty()) {
            o = pa_context_get_sink_info_by_name(
                c, self->d->defaultSinkName.toUtf8().constData(), sinkInfo,
                ud);
            if (o)
                pa_operation_unref(o);
        }
        if (!self->d->defaultSourceName.isEmpty()) {
            o = pa_context_get_source_info_by_name(
                c, self->d->defaultSourceName.toUtf8().constData(),
                sourceInfo, ud);
            if (o)
                pa_operation_unref(o);
        }
        o = pa_context_get_sink_info_list(c, sinkList, ud);
        if (o)
            pa_operation_unref(o);
    }

    static void sinkInfo(pa_context *, const pa_sink_info *i, int eol,
                         void *ud)
    {
        if (eol || !i)
            return;
        auto *self = static_cast<PulseBackend *>(ud);
        self->d->sinkChannels = i->volume.channels;
        const qreal vol =
            (qreal)pa_cvolume_avg(&i->volume) / (qreal)PA_VOLUME_NORM;
        const bool muted = i->mute != 0;
        QMetaObject::invokeMethod(
            self,
            [self, vol, muted]() {
                self->hasSink = true;
                self->sinkVolume = vol;
                self->sinkMuted = muted;
                emit self->changed();
            },
            Qt::QueuedConnection);
    }

    static void sourceInfo(pa_context *, const pa_source_info *i, int eol,
                           void *ud)
    {
        if (eol || !i)
            return;
        auto *self = static_cast<PulseBackend *>(ud);
        self->d->sourceChannels = i->volume.channels;
        const qreal vol =
            (qreal)pa_cvolume_avg(&i->volume) / (qreal)PA_VOLUME_NORM;
        const bool muted = i->mute != 0;
        QMetaObject::invokeMethod(
            self,
            [self, vol, muted]() {
                self->hasSource = true;
                self->sourceVolume = vol;
                self->sourceMuted = muted;
                emit self->changed();
            },
            Qt::QueuedConnection);
    }

    static void sinkList(pa_context *, const pa_sink_info *i, int eol,
                         void *ud)
    {
        auto *self = static_cast<PulseBackend *>(ud);
        static thread_local QVector<PulseBackend::Sink> pending;
        if (eol) {
            const QVector<PulseBackend::Sink> list = pending;
            pending.clear();
            const QString def = self->d->defaultSinkName;
            QMetaObject::invokeMethod(
                self,
                [self, list, def]() {
                    QVector<PulseBackend::Sink> l = list;
                    for (PulseBackend::Sink &s : l)
                        s.isDefault = (s.name == def);
                    self->sinks = l;
                    emit self->changed();
                },
                Qt::QueuedConnection);
            return;
        }
        if (!i)
            return;
        PulseBackend::Sink s;
        s.name = QString::fromUtf8(i->name ? i->name : "");
        s.description =
            QString::fromUtf8(i->description ? i->description : "");
        pending.push_back(s);
    }
};

PulseBackend *PulseBackend::instance()
{
    static PulseBackend *s = new PulseBackend();
    return s;
}

PulseBackend::PulseBackend(QObject *parent) : QObject(parent), d(new Priv)
{
    start();
}

void PulseBackend::start()
{
    d->loop = pa_threaded_mainloop_new();
    newContext();
    pa_threaded_mainloop_start(d->loop);
}

/* Caller holds the mainloop lock, or the loop is not running yet. */
void PulseBackend::newContext()
{
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(d->loop);
    d->ctx = pa_context_new(api, "fanshell-qs-shell");
    pa_context_set_state_callback(d->ctx, PulseCallbacks::contextState, this);
    pa_context_connect(d->ctx, nullptr, PA_CONTEXT_NOFAIL, nullptr);
}

void PulseBackend::connectionLost()
{
    if (hasSink || hasSource || !sinks.isEmpty()) {
        hasSink = false;
        hasSource = false;
        sinks.clear();
        emit changed();
    }
    if (d->reconnectPending)
        return;
    d->reconnectPending = true;
    /* A short pause lets a restarting server finish coming back up. */
    QTimer::singleShot(1000, this, [this]() {
        d->reconnectPending = false;
        pa_threaded_mainloop_lock(d->loop);
        const pa_context_state_t state = pa_context_get_state(d->ctx);
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            pa_context_set_state_callback(d->ctx, nullptr, nullptr);
            pa_context_set_subscribe_callback(d->ctx, nullptr, nullptr);
            pa_context_disconnect(d->ctx);
            pa_context_unref(d->ctx);
            d->defaultSinkName.clear();
            d->defaultSourceName.clear();
            newContext();
        }
        pa_threaded_mainloop_unlock(d->loop);
    });
}

void PulseBackend::setSinkVolume(qreal v)
{
    v = qMax<qreal>(0, qMin<qreal>(1, v));
    if (!hasSink)
        return;
    /* defaultSinkName is written on the PA thread; read it under the lock. */
    pa_threaded_mainloop_lock(d->loop);
    if (d->defaultSinkName.isEmpty()) {
        pa_threaded_mainloop_unlock(d->loop);
        return;
    }
    pa_cvolume cv;
    pa_cvolume_set(&cv, d->sinkChannels,
                   (pa_volume_t)qRound64(v * PA_VOLUME_NORM));
    pa_operation *o = pa_context_set_sink_volume_by_name(
        d->ctx, d->defaultSinkName.toUtf8().constData(), &cv, nullptr,
        nullptr);
    if (o)
        pa_operation_unref(o);
    pa_threaded_mainloop_unlock(d->loop);
    sinkVolume = v;
    emit changed();
}

void PulseBackend::setSinkMuted(bool m)
{
    if (!hasSink)
        return;
    /* defaultSinkName is written on the PA thread; read it under the lock. */
    pa_threaded_mainloop_lock(d->loop);
    if (d->defaultSinkName.isEmpty()) {
        pa_threaded_mainloop_unlock(d->loop);
        return;
    }
    pa_operation *o = pa_context_set_sink_mute_by_name(
        d->ctx, d->defaultSinkName.toUtf8().constData(), m ? 1 : 0, nullptr,
        nullptr);
    if (o)
        pa_operation_unref(o);
    pa_threaded_mainloop_unlock(d->loop);
    sinkMuted = m;
    emit changed();
}

void PulseBackend::setSourceVolume(qreal v)
{
    v = qMax<qreal>(0, qMin<qreal>(1, v));
    if (!hasSource)
        return;
    pa_threaded_mainloop_lock(d->loop);
    if (d->defaultSourceName.isEmpty()) {
        pa_threaded_mainloop_unlock(d->loop);
        return;
    }
    pa_cvolume cv;
    pa_cvolume_set(&cv, d->sourceChannels,
                   (pa_volume_t)qRound64(v * PA_VOLUME_NORM));
    pa_operation *o = pa_context_set_source_volume_by_name(
        d->ctx, d->defaultSourceName.toUtf8().constData(), &cv, nullptr,
        nullptr);
    if (o)
        pa_operation_unref(o);
    pa_threaded_mainloop_unlock(d->loop);
    sourceVolume = v;
    emit changed();
}

void PulseBackend::setSourceMuted(bool m)
{
    if (!hasSource)
        return;
    pa_threaded_mainloop_lock(d->loop);
    if (d->defaultSourceName.isEmpty()) {
        pa_threaded_mainloop_unlock(d->loop);
        return;
    }
    pa_operation *o = pa_context_set_source_mute_by_name(
        d->ctx, d->defaultSourceName.toUtf8().constData(), m ? 1 : 0, nullptr,
        nullptr);
    if (o)
        pa_operation_unref(o);
    pa_threaded_mainloop_unlock(d->loop);
    sourceMuted = m;
    emit changed();
}

void PulseBackend::setDefaultSink(const QString &name)
{
    pa_threaded_mainloop_lock(d->loop);
    pa_operation *o = pa_context_set_default_sink(
        d->ctx, name.toUtf8().constData(), nullptr, nullptr);
    if (o)
        pa_operation_unref(o);
    pa_threaded_mainloop_unlock(d->loop);
}

/* ----------------------------------------------------------- ClickableText */

ClickableText::ClickableText(QWidget *parent) : TextItem(parent)
{
    setCursor(Qt::PointingHandCursor);
}

void ClickableText::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        m_pressed = true;
}

void ClickableText::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_pressed && e->button() == Qt::LeftButton
            && rect().contains(e->pos()))
        emit clicked();
    m_pressed = false;
}
