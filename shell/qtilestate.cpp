#include "qtilestate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

#include "qtileipc.h"

namespace {

constexpr int retryMs = 1000;

/* Evaluated by qtile with `self` as its root object; answers the snapshot
 * refresh() reads. The lambda gives the comprehensions a scope that sees q. */
const char snapshotCode[] = R"PY((lambda q: __import__("json").dumps({
    "current": q.current_screen.index,
    "screens": [
        {"index": s.index, "x": s.x, "y": s.y, "w": s.width, "h": s.height,
         "group": s.group.name if s.group else None}
        for s in q.screens],
    "groups": [
        {"name": g.name,
         "label": g.label or g.name,
         "affinity": g.screen_affinity
             if isinstance(g.screen_affinity, int) else None,
         "scratchpad": type(g).__name__ == "ScratchPad",
         "windows": len(g.windows),
         "urgent": any(w.urgent for w in g.windows),
         "fullscreen": any(w.fullscreen for w in g.windows),
         "layout": g.layout.name if g.layout else "",
         "title": (g.current_window.name or "") if g.current_window else ""}
        for g in q.groups]}))(self))PY";

/* Show window XID and focus it. A window in a hidden group (a special
 * workspace or a ScratchPad) is brought to the focused screen's group; one in
 * a group that is not showing is shown on its own screen (its affinity), or
 * the focused one. Answers True when the window exists. */
const char focusCode[] = R"PY((lambda q, w: False if w is None or w.group is None else (
    lambda g, hidden: (
        w.togroup(q.current_group.name) if hidden else
        g.toscreen(g.screen_affinity
                   if isinstance(g.screen_affinity, int)
                   and g.screen_affinity < len(q.screens) else None)
        if g.screen is None else None,
        q.focus_screen(w.group.screen.index, warp=False),
        w.group.focus(w, warp=False),
        True)[-1])(
    w.group,
    w.group.name.startswith("special:") or type(w.group).__name__ == "ScratchPad")
)(self, self.windows_map.get(%1)))PY";

bool hiddenGroup(const QJsonObject &g)
{
    return g.value(QStringLiteral("scratchpad")).toBool()
           || g.value(QStringLiteral("name"))
                  .toString()
                  .startsWith(QLatin1String("special:"));
}

} /* namespace */

QtileState::QtileState(QObject *parent) : WmState(parent)
{
    m_retry.setSingleShot(true);
    m_retry.setInterval(retryMs);
    connect(&m_retry, &QTimer::timeout, this, &QtileState::scheduleRefresh);

    /* Don't wait for the first hook to paint something. */
    refresh();
}

WmState::Features QtileState::features() const
{
    Features f;
    f.cycleLayout = true;
    return f;
}

QString QtileState::unavailableHint() const
{
    if (connected)
        return QString();
    if (!QtileIpc::available())
        return QStringLiteral("no qtile IPC socket at %1 -- is qtile running "
                              "on this $DISPLAY?")
            .arg(QtileIpc::socketPath());
    return QStringLiteral("qtile is not answering on %1.")
        .arg(QtileIpc::socketPath());
}

void QtileState::viewWorkspace(int monitorId, int index)
{
    const QStringList groups = m_groups.value(monitorId);
    if (index < 0 || index >= groups.size())
        return;
    const QJsonArray group{QJsonArray{QStringLiteral("group"), groups[index]}};
    QtileIpc::call(group, QStringLiteral("toscreen"), QJsonArray{monitorId});
    QtileIpc::call(QJsonArray(), QStringLiteral("to_screen"),
                   QJsonArray{monitorId});
}

void QtileState::cycleLayout(int monitorId)
{
    const QString group = m_shown.value(monitorId);
    if (!group.isEmpty())
        QtileIpc::call(QJsonArray(), QStringLiteral("next_layout"),
                       QJsonArray{group});
}

QVector<quintptr> QtileState::windows()
{
    QVector<quintptr> out;
    bool ok = false;
    const QJsonArray list =
        QtileIpc::call(QJsonArray(), QStringLiteral("windows"), QJsonArray(), &ok)
            .toArray();
    if (!ok)
        return out;
    for (const QJsonValue &v : list) {
        const double id = v.toObject().value(QStringLiteral("id")).toDouble(-1);
        if (id > 0)
            out << quintptr(id);
    }
    return out;
}

bool QtileState::focusWindow(quintptr window)
{
    bool ok = false;
    const QString answer =
        QtileIpc::eval(QString::fromLatin1(focusCode).arg(window), &ok);
    return ok && answer == QLatin1String("True");
}

void QtileState::refresh()
{
    bool ok = false;
    const QString raw = QtileIpc::eval(QString::fromLatin1(snapshotCode), &ok);
    const QJsonObject snap =
        ok ? QJsonDocument::fromJson(raw.toUtf8()).object() : QJsonObject();
    if (snap.isEmpty()) {
        setDisconnected();
        m_retry.start();
        return;
    }

    const QJsonArray screens = snap.value(QStringLiteral("screens")).toArray();
    const int current = snap.value(QStringLiteral("current")).toInt(-1);

    /* The groups that can be on the bar, in config order. */
    QVector<QJsonObject> groups;
    QHash<QString, QJsonObject> byName;
    bool perMonitor = false;
    for (const QJsonValue &v : snap.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject g = v.toObject();
        byName.insert(g.value(QStringLiteral("name")).toString(), g);
        if (hiddenGroup(g))
            continue;
        groups << g;
        if (!g.value(QStringLiteral("affinity")).isNull())
            perMonitor = true;
    }

    /* Each monitor's workspaces, as group names. */
    QHash<int, QStringList> own;
    for (const QJsonValue &v : screens) {
        const int index = v.toObject().value(QStringLiteral("index")).toInt();
        QStringList names;
        for (const QJsonObject &g : groups) {
            const QJsonValue affinity = g.value(QStringLiteral("affinity"));
            if (!perMonitor || (!affinity.isNull() && affinity.toInt() == index))
                names << g.value(QStringLiteral("name")).toString();
        }
        own.insert(index, names);
    }

    /* One label row for every monitor: per monitor, the labels of the
     * longest list (fanqtile-qs labels every monitor's set 1-5). */
    QStringList labels;
    for (const QStringList &names : std::as_const(own)) {
        if (names.size() <= labels.size())
            continue;
        labels.clear();
        for (const QString &name : names)
            labels << byName.value(name).value(QStringLiteral("label")).toString();
    }

    QVector<Monitor> mons;
    QHash<int, QString> shown;
    for (const QJsonValue &v : screens) {
        const QJsonObject s = v.toObject();
        Monitor m;
        m.id = s.value(QStringLiteral("index")).toInt();
        m.x = s.value(QStringLiteral("x")).toInt();
        m.y = s.value(QStringLiteral("y")).toInt();
        m.w = s.value(QStringLiteral("w")).toInt();
        m.h = s.value(QStringLiteral("h")).toInt();
        m.focused = m.id == current;

        const QString showing = s.value(QStringLiteral("group")).toString();
        shown.insert(m.id, showing);
        const QJsonObject group = byName.value(showing);
        m.layout = group.value(QStringLiteral("layout")).toString();
        m.title = group.value(QStringLiteral("title")).toString();
        m.fullscreen = group.value(QStringLiteral("fullscreen")).toBool();

        const QStringList names = own.value(m.id);
        for (int i = 0; i < names.size() && i < 32; ++i) {
            const quint32 bit = 1u << i;
            const QJsonObject g = byName.value(names[i]);
            if (names[i] == showing)
                m.selected |= bit;
            else if (!perMonitor) {
                for (const QJsonValue &o : screens)
                    if (o.toObject().value(QStringLiteral("group")).toString()
                            == names[i])
                        m.elsewhere |= bit;
            }
            if (g.value(QStringLiteral("windows")).toInt() > 0)
                m.occupied |= bit;
            if (g.value(QStringLiteral("urgent")).toBool())
                m.urgent |= bit;
        }
        mons.push_back(m);
    }
    std::sort(mons.begin(), mons.end(),
              [](const Monitor &a, const Monitor &b) { return a.id < b.id; });

    m_groups = own;
    m_shown = shown;
    publish(mons, labels);
}
