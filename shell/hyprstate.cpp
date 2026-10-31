#include "hyprstate.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <limits>

namespace {

/* Workspaces per output when no rule pins any to it. */
constexpr int workspacesPerMonitor = 5;

QString normalizedAddress(QString address)
{
    address = address.trimmed().toLower();
    if (!address.isEmpty() && !address.startsWith(QLatin1String("0x")))
        address.prepend(QStringLiteral("0x"));
    return address;
}

/* Hyprland spells "no submap" two ways: the `submap` command answers
 * "default", the event stream sends an empty payload. */
QString normalizeSubmap(const QString &raw)
{
    const QString t = raw.trimmed();
    return t == QLatin1String("default") ? QString() : t;
}

int workspaceId(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toObject().value(QStringLiteral("id")).toInt();
}

bool focusAddress(const QString &address)
{
    return HyprIpc::dispatch(
        QStringLiteral("hl.dsp.focus({ window = \"address:%1\" })").arg(address),
        QStringLiteral("focuswindow address:%1").arg(address));
}

} /* namespace */

HyprState::HyprState(QObject *parent) : WmState(parent)
{
    /* Animated titles can change ten times a second; keep their trailing
     * edge apart from structural changes, which stay immediate. */
    m_titleDebounce.setSingleShot(true);
    m_titleDebounce.setInterval(300);
    connect(&m_titleDebounce, &QTimer::timeout, this,
            &HyprState::applyTitleUpdates);

    connect(&m_events, &HyprEventStream::hyprEvent, this,
            [this](const QString &name, const QString &data) {
                if (name == QLatin1String("configreloaded")) {
                    refreshConfig();
                    emit configReloaded();
                }
                if (name.startsWith(QLatin1String("monitoradded"))
                        || name.startsWith(QLatin1String("monitorremoved")))
                    emit outputsChanged();
                /* Self-contained: take it at its word. */
                if (name == QLatin1String("submap")) {
                    const QString next = normalizeSubmap(data);
                    if (submap != next) {
                        submap = next;
                        emit changed();
                    }
                    return;
                }
                /* 0.56 emits both generations; the v2 ones carry the
                 * window address, so the old ones add nothing. */
                if (name == QLatin1String("windowtitle")
                        || name == QLatin1String("activewindow"))
                    return;
                if (name == QLatin1String("windowtitlev2")) {
                    queueTitleUpdate(data);
                    return;
                }
                if (name == QLatin1String("activewindowv2")
                        && normalizedAddress(data) == m_activeClient)
                    return; /* repeated for a title-only change */
                m_titleDebounce.stop();
                m_pendingTitles.clear();
                scheduleRefresh();
            });
    connect(&m_events, &HyprEventStream::connected, this, [this]() {
        refreshConfig();
        scheduleRefresh();
    });
    m_events.start();

    if (HyprIpc::available()) {
        refreshConfig();
        scheduleRefresh();
    }
}

WmState::Features HyprState::features() const
{
    Features f;
    f.submaps = true;
    return f;
}

QString HyprState::unavailableHint() const
{
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return QStringLiteral("HYPRLAND_INSTANCE_SIGNATURE is not set -- the "
                              "shell was not started by Hyprland.");
    if (!HyprIpc::available())
        return QStringLiteral("Hyprland's IPC socket is not there yet.");
    return QString();
}

QString HyprState::primaryMonitor() const
{
    const Monitor *best = nullptr;
    int lowest = 0;
    for (const Monitor &m : monitors) {
        const QVector<int> block = m_blocks.value(m.id);
        if (block.isEmpty())
            continue;
        if (!best || block.first() < lowest) {
            best = &m;
            lowest = block.first();
        }
    }
    return best ? best->name : WmState::primaryMonitor();
}

void HyprState::viewWorkspace(int monitorId, int index)
{
    const QVector<int> block = m_blocks.value(monitorId);
    if (index < 0 || index >= block.size())
        return;
    /* `focus{workspace=N}` also moves focus to the output that owns N, which
     * is what clicking another monitor's bar should do. */
    const int id = block[index];
    HyprIpc::dispatch(QStringLiteral("hl.dsp.focus({ workspace = %1 })").arg(id),
                      QStringLiteral("workspace %1").arg(id));
}

void HyprState::resetSubmap()
{
    if (submap.isEmpty())
        return;
    HyprIpc::dispatch(QStringLiteral("hl.dsp.submap(\"reset\")"),
                      QStringLiteral("submap reset"));
}

QVector<quintptr> HyprState::windows()
{
    QVector<quintptr> out;
    for (const QJsonValue &v : HyprIpc::requestArray("clients")) {
        bool ok = false;
        const quintptr a = v.toObject()
                               .value(QStringLiteral("address"))
                               .toString()
                               .toULongLong(&ok, 16);
        if (ok)
            out << a;
    }
    return out;
}

bool HyprState::focusWindow(quintptr window)
{
    return focusAddress(QStringLiteral("0x%1").arg(window, 0, 16));
}

/* Hyprland knows each client's pid, so no property reads. */
bool HyprState::focusPid(uint pid)
{
    if (!pid)
        return false;
    QString address;
    int matches = 0;
    for (const QJsonValue &v : HyprIpc::requestArray("clients")) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("pid")).toInt() != int(pid))
            continue;
        address = normalizedAddress(o.value(QStringLiteral("address")).toString());
        matches++;
    }
    return matches == 1 && !address.isEmpty() && focusAddress(address);
}

void HyprState::queueTitleUpdate(const QString &data)
{
    const int comma = data.indexOf(QLatin1Char(','));
    if (comma <= 0) {
        /* Unexpected payload: the full refresh, still debounced. */
        m_pendingTitles.insert(QString(), QString());
        m_titleDebounce.start();
        return;
    }
    const QString address = normalizedAddress(data.left(comma));
    const QString title = data.mid(comma + 1);
    if (m_knownClients.contains(address)) {
        const auto it = m_shownClientMonitor.constFind(address);
        if (it != m_shownClientMonitor.constEnd() && !updateTitle(*it, title))
            scheduleRefresh();
        return;
    }
    /* A new client can name itself before the refresh that lists it. */
    m_pendingTitles.insert(address, title);
    m_titleDebounce.start();
}

void HyprState::applyTitleUpdates()
{
    bool fallback = false;
    for (auto it = m_pendingTitles.constBegin(); it != m_pendingTitles.constEnd();
         ++it) {
        if (it.key().isEmpty() || !m_knownClients.contains(it.key())) {
            fallback = true;
            break;
        }
    }
    if (fallback) {
        m_pendingTitles.clear();
        refresh();
        return;
    }
    for (auto it = m_pendingTitles.constBegin(); it != m_pendingTitles.constEnd();
         ++it) {
        const auto mon = m_shownClientMonitor.constFind(it.key());
        if (mon != m_shownClientMonitor.constEnd())
            updateTitle(*mon, it.value());
    }
    m_pendingTitles.clear();
}

void HyprState::refreshConfig()
{
    const QString next =
        normalizeSubmap(QString::fromUtf8(HyprIpc::request("submap")));
    const bool submapChanged = next != submap;
    submap = next;

    /* Only plain numbered workspaces pinned to a named output take part in
     * the fixed row; name: and special: rules have no button. */
    m_rules.clear();
    for (const QJsonValue &v : HyprIpc::requestArray("workspacerules")) {
        const QJsonObject o = v.toObject();
        const QString monitor = o.value(QStringLiteral("monitor")).toString();
        if (monitor.isEmpty())
            continue;
        bool ok = false;
        const int id =
            o.value(QStringLiteral("workspaceString")).toString().toInt(&ok);
        if (!ok || id <= 0)
            continue;
        QVector<int> &ids = m_rules[monitor];
        if (!ids.contains(id))
            ids.push_back(id);
    }
    for (auto it = m_rules.begin(); it != m_rules.end(); ++it)
        std::sort(it->begin(), it->end());
    if (submapChanged)
        emit changed();
}

void HyprState::refresh()
{
    if (!HyprIpc::available()) {
        setDisconnected();
        return;
    }

    struct Ws {
        QString name;
        int windows = 0;
        bool fullscreen = false;
    };
    QHash<int, Ws> existing;
    for (const QJsonValue &v : HyprIpc::requestArray("workspaces")) {
        const QJsonObject o = v.toObject();
        const int id = o.value(QStringLiteral("id")).toInt();
        if (id <= 0)
            continue; /* special workspaces: negative ids, no button */
        Ws w;
        w.name = o.value(QStringLiteral("name")).toString();
        w.windows = o.value(QStringLiteral("windows")).toInt();
        w.fullscreen = o.value(QStringLiteral("hasfullscreen")).toBool();
        existing.insert(id, w);
    }

    QVector<Monitor> mons;
    QHash<int, int> active; /* monitor id -> its workspace */
    for (const QJsonValue &v : HyprIpc::requestArray("monitors")) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("disabled")).toBool())
            continue;
        Monitor m;
        m.id = o.value(QStringLiteral("id")).toInt();
        m.name = o.value(QStringLiteral("name")).toString();
        m.x = o.value(QStringLiteral("x")).toInt();
        m.y = o.value(QStringLiteral("y")).toInt();
        m.w = o.value(QStringLiteral("width")).toInt();
        m.h = o.value(QStringLiteral("height")).toInt();
        m.focused = o.value(QStringLiteral("focused")).toBool();
        active.insert(m.id, workspaceId(o, "activeWorkspace"));
        mons.push_back(m);
    }
    /* Connection order is not stable across hotplug; id order keeps the
     * fallback blocks where they were. */
    std::sort(mons.begin(), mons.end(),
              [](const Monitor &a, const Monitor &b) { return a.id < b.id; });

    QHash<int, QVector<int>> blocks;
    for (int slot = 0; slot < mons.size(); ++slot) {
        Monitor &m = mons[slot];
        QVector<int> block = m_rules.value(m.name);
        if (block.isEmpty())
            for (int i = 0; i < workspacesPerMonitor; ++i)
                block << slot * workspacesPerMonitor + i + 1;
        /* A workspace moved onto this output from outside its block would
         * otherwise leave nothing highlighted. */
        const int shown = active.value(m.id);
        if (shown > 0 && !block.contains(shown)) {
            block << shown;
            std::sort(block.begin(), block.end());
        }
        for (int i = 0; i < block.size() && i < 32; ++i) {
            const quint32 bit = 1u << i;
            const int id = block[i];
            const Ws w = existing.value(id);
            if (id == shown)
                m.selected |= bit;
            if (w.windows > 0)
                m.occupied |= bit;
            const QString label = !w.name.isEmpty() && w.name != QString::number(id)
                                      ? w.name
                                      : QString::number(id);
            m.labels << label;
        }
        m.fullscreen = existing.value(shown).fullscreen;
        blocks.insert(m.id, block);
    }

    /* Each panel's title: the most recently focused client on the workspace
     * its output shows, which is the one Hyprland would focus there. */
    QHash<int, int> bestFocus;
    QHash<int, QString> bestAddress;
    QSet<QString> known;
    QString activeClient;
    int activeFocus = std::numeric_limits<int>::max();
    for (const QJsonValue &v : HyprIpc::requestArray("clients")) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("hidden")).toBool())
            continue;
        const QString address =
            normalizedAddress(o.value(QStringLiteral("address")).toString());
        if (address.isEmpty())
            continue;
        known.insert(address);
        const int focus = o.value(QStringLiteral("focusHistoryID")).toInt();
        if (focus < activeFocus) {
            activeFocus = focus;
            activeClient = address;
        }
        const int monId = o.value(QStringLiteral("monitor")).toInt();
        if (active.value(monId, -1) != workspaceId(o, "workspace"))
            continue;
        if (focus < bestFocus.value(monId, std::numeric_limits<int>::max())) {
            bestFocus.insert(monId, focus);
            bestAddress.insert(monId, address);
            for (Monitor &m : mons)
                if (m.id == monId)
                    m.title = o.value(QStringLiteral("title")).toString();
        }
    }

    m_shownClientMonitor.clear();
    for (auto it = bestAddress.constBegin(); it != bestAddress.constEnd(); ++it)
        m_shownClientMonitor.insert(it.value(), it.key());
    m_knownClients = known;
    m_activeClient = activeClient;
    m_blocks = blocks;

    publish(mons, QStringList());
}
