#include "oxwmstate.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

OxwmState::OxwmState(QObject *parent) : WmState(parent)
{
    connect(&m_events, &OxwmEventStream::wmEvent, this,
            [this](const QString &name, const QString &data) {
                if (name == QLatin1String("title")) {
                    applyTitle(data);
                    return;
                }
                scheduleRefresh();
            });
    connect(&m_events, &OxwmEventStream::connected, this,
            &OxwmState::scheduleRefresh);
    connect(&m_events, &OxwmEventStream::disconnected, this,
            &OxwmState::scheduleRefresh);

    m_events.start();

    /* Don't wait for the first event to paint something. */
    if (OxwmIpc::available())
        refresh();
}

WmState::Features OxwmState::features() const
{
    Features f;
    f.toggleWorkspaces = true;
    f.cycleLayout = true;
    return f;
}

QString OxwmState::unavailableHint() const
{
    if (OxwmIpc::available())
        return QString();
    return QStringLiteral(
        "no oxwm IPC socket for this $DISPLAY -- is oxwm running, and built "
        "with scripts/build-patched-oxwm?");
}

void OxwmState::viewWorkspace(int monitorId, int index)
{
    OxwmIpc::command(QStringLiteral("focusmonitor %1;dispatch view_tag %2")
                         .arg(monitorId)
                         .arg(index));
}

void OxwmState::toggleWorkspace(int monitorId, int index)
{
    OxwmIpc::command(
        QStringLiteral("focusmonitor %1;dispatch toggle_view_tag %2")
            .arg(monitorId)
            .arg(index));
}

void OxwmState::cycleLayout(int monitorId)
{
    OxwmIpc::command(QStringLiteral("focusmonitor %1;dispatch cycle_layout")
                         .arg(monitorId));
}

QVector<quintptr> OxwmState::windows()
{
    QVector<quintptr> out;
    for (const QJsonValue &v : OxwmIpc::requestArray("clients")) {
        bool ok = false;
        const quintptr id = v.toObject()
                                .value(QStringLiteral("window"))
                                .toString()
                                .toULongLong(&ok, 0);
        if (ok)
            out << id;
    }
    return out;
}

bool OxwmState::focusWindow(quintptr window)
{
    return OxwmIpc::command(
        QStringLiteral("focuswindow 0x%1").arg(window, 0, 16));
}

/* title>>MON,WINDOW,TITLE -- the title may itself contain commas. */
void OxwmState::applyTitle(const QString &data)
{
    const int first = data.indexOf(QLatin1Char(','));
    const int second = first < 0 ? -1 : data.indexOf(QLatin1Char(','), first + 1);
    bool ok = false;
    const int id = first < 0 ? -1 : data.left(first).toInt(&ok);
    /* An unexpected payload, or a monitor not seen yet: a full read. */
    if (!ok || second < 0 || !updateTitle(id, data.mid(second + 1)))
        scheduleRefresh();
}

void OxwmState::refresh()
{
    const QJsonObject state = OxwmIpc::requestJson("state").object();
    if (state.isEmpty()) {
        setDisconnected();
        return;
    }

    QStringList tags;
    for (const QJsonValue &v : state.value(QStringLiteral("tags")).toArray())
        tags << v.toString();

    QVector<Monitor> mons;
    for (const QJsonValue &v :
         state.value(QStringLiteral("monitors")).toArray()) {
        const QJsonObject o = v.toObject();
        Monitor m;
        m.id = o.value(QStringLiteral("id")).toInt();
        m.x = o.value(QStringLiteral("x")).toInt();
        m.y = o.value(QStringLiteral("y")).toInt();
        m.w = o.value(QStringLiteral("width")).toInt();
        m.h = o.value(QStringLiteral("height")).toInt();
        m.focused = o.value(QStringLiteral("focused")).toBool();
        m.selected = quint32(o.value(QStringLiteral("selectedTags")).toDouble());
        m.occupied = quint32(o.value(QStringLiteral("occupiedTags")).toDouble());
        m.urgent = quint32(o.value(QStringLiteral("urgentTags")).toDouble());
        m.layout = o.value(QStringLiteral("layout")).toString();
        m.title = o.value(QStringLiteral("title")).toString();
        mons.push_back(m);
    }
    publish(mons, tags);
}
