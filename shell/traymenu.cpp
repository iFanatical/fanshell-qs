#include "traymenu.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QEnterEvent>
#include <QHash>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>

#include <functional>

#include "theme.h"

static const char *kMenuIface = "com.canonical.dbusmenu";

/* ---------------------------------------------------------- DBusMenuClient */

DBusMenuClient::DBusMenuClient(const QString &service, const QString &path,
                               QObject *parent)
    : QObject(parent), m_service(service), m_path(path)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(m_service, m_path, QString::fromLatin1(kMenuIface),
                QStringLiteral("LayoutUpdated"), this, SIGNAL(updated()));
    bus.connect(m_service, m_path, QString::fromLatin1(kMenuIface),
                QStringLiteral("ItemsPropertiesUpdated"), this,
                SIGNAL(updated()));
}

static QString stripMnemonics(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); i++) {
        if (in[i] == QLatin1Char('_')) {
            if (i + 1 < in.size() && in[i + 1] == QLatin1Char('_')) {
                out += QLatin1Char('_');
                i++;
            }
            continue;
        }
        out += in[i];
    }
    return out;
}

static DBusMenuClient::Entry entryFromProps(int id, const QVariantMap &props,
                                            bool hasChildren)
{
    DBusMenuClient::Entry e;
    e.id = id;
    e.label = stripMnemonics(props.value(QStringLiteral("label")).toString());
    e.enabled = props.value(QStringLiteral("enabled"), true).toBool();
    e.visible = props.value(QStringLiteral("visible"), true).toBool();
    e.separator = props.value(QStringLiteral("type")).toString()
                  == QLatin1String("separator");
    const QString toggle =
        props.value(QStringLiteral("toggle-type")).toString();
    e.buttonType = toggle == QLatin1String("checkmark")
                       ? 1
                       : (toggle == QLatin1String("radio") ? 2 : 0);
    e.checked = props.value(QStringLiteral("toggle-state")).toInt() == 1;
    e.iconName = props.value(QStringLiteral("icon-name")).toString();
    const QByteArray png =
        props.value(QStringLiteral("icon-data")).toByteArray();
    if (!png.isEmpty())
        e.iconData = QImage::fromData(png);
    e.hasChildren = hasChildren
                    || props.value(QStringLiteral("children-display"))
                               .toString()
                           == QLatin1String("submenu");
    return e;
}

void DBusMenuClient::fetchLayout(int parentId)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, QString::fromLatin1(kMenuIface),
        QStringLiteral("GetLayout"));
    msg << parentId << 1 << QStringList();
    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, parentId](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusMessage reply = w->reply();
                if (reply.type() != QDBusMessage::ReplyMessage
                        || reply.arguments().size() < 2)
                    return;
                QVariant layoutV = reply.arguments().at(1);
                if (layoutV.canConvert<QDBusVariant>())
                    layoutV = layoutV.value<QDBusVariant>().variant();
                const QDBusArgument arg = layoutV.value<QDBusArgument>();
                /* (ia{sv}av): id, props, children */
                QVector<Entry> entries;
                int rootId = 0;
                QVariantMap rootProps;
                arg.beginStructure();
                arg >> rootId >> rootProps;
                arg.beginArray();
                while (!arg.atEnd()) {
                    QDBusVariant childV;
                    arg >> childV;
                    const QDBusArgument child =
                        childV.variant().value<QDBusArgument>();
                    int cid = 0;
                    QVariantMap cprops;
                    child.beginStructure();
                    child >> cid >> cprops;
                    /* child's own children (depth 1: may be empty even if it
                     * has a submenu — children-display covers that) */
                    bool kids = false;
                    child.beginArray();
                    while (!child.atEnd()) {
                        QDBusVariant sub;
                        child >> sub;
                        kids = true;
                    }
                    child.endArray();
                    child.endStructure();
                    entries.push_back(entryFromProps(cid, cprops, kids));
                }
                arg.endArray();
                arg.endStructure();
                emit layout(parentId, entries);
            });
}

void DBusMenuClient::aboutToShow(int parentId)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, QString::fromLatin1(kMenuIface),
        QStringLiteral("AboutToShow"));
    msg << parentId;
    QDBusConnection::sessionBus().asyncCall(msg);
}

void DBusMenuClient::triggerEvent(int id)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, QString::fromLatin1(kMenuIface),
        QStringLiteral("Event"));
    msg << id << QStringLiteral("clicked")
        << QVariant::fromValue(QDBusVariant(QString()))
        << (uint)QDateTime::currentSecsSinceEpoch();
    QDBusConnection::sessionBus().asyncCall(msg);
}

/* ----------------------------------------------------------------- MenuRow */

namespace {

class MenuRow : public QWidget {
public:
    MenuRow(const DBusMenuClient::Entry &e, QWidget *parent)
        : QWidget(parent), m_e(e)
    {
        setFixedHeight(e.separator ? 7 : 26);
        if (!e.separator && e.enabled)
            setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

    std::function<void(MenuRow *, const DBusMenuClient::Entry &)> onClick;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        if (m_e.separator) {
            p.fillRect(QRect(4, height() / 2, width() - 8, 1), Theme::border);
            return;
        }
        Theme::paintRect(p, rect(),
                         (m_hover && m_e.enabled) ? Theme::surfaceHover
                                                  : Theme::transparent,
                         Theme::smallRadius);
        int x = 8;
        /* check/radio column (the mark glyphs are empty strings in the
         * current theme; the 14px column is still reserved) */
        if (m_e.buttonType != 0)
            x += 14;
        /* icon */
        int iconW = 0;
        QImage img = m_e.iconData;
        if (img.isNull() && !m_e.iconName.isEmpty()) {
            const QIcon ic = QIcon::fromTheme(m_e.iconName);
            if (!ic.isNull())
                img = ic.pixmap(16, 16).toImage();
        }
        if (!img.isNull() || !m_e.iconName.isEmpty())
            iconW = 16;
        if (!img.isNull()) {
            QSizeF sz = img.size();
            sz.scale(16, 16, Qt::KeepAspectRatio);
            const QRectF tgt(x + (m_e.iconName.isEmpty() ? 0 : 6)
                                 + (16 - sz.width()) / 2.0,
                             (height() - sz.height()) / 2.0, sz.width(),
                             sz.height());
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(tgt, img);
        }
        if (iconW > 0)
            x += 6 + 16;
        /* label */
        const int right = width() - 6; /* arrow slot (empty glyph) at right */
        p.setFont(Theme::font(Theme::smallFontSize));
        p.setPen(m_e.enabled ? Theme::text : Theme::textMuted);
        const QFontMetrics fm(p.font());
        const int tw = right - (x + 8);
        p.drawText(QRect(x + 8, 0, tw, height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_e.label, Qt::ElideRight, tw));
    }

    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            m_pressed = true;
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_pressed && e->button() == Qt::LeftButton
                && rect().contains(e->pos()) && m_e.enabled && !m_e.separator
                && onClick)
            onClick(this, m_e);
        m_pressed = false;
    }

private:
    DBusMenuClient::Entry m_e;
    bool m_hover = false;
    bool m_pressed = false;
};

} /* namespace */

/* ---------------------------------------------------------------- TrayMenu */

TrayMenu::TrayMenu(const QString &service, const QString &path,
                   QWidget *anchor)
    : ShellPopup(anchor, 240), m_parentId(0), m_topLevel(true)
{
    m_client = new DBusMenuClient(service, path, this);
    setAnchorOffset(QPoint(0, anchor ? anchor->height() : 0));
    init();
}

TrayMenu::TrayMenu(DBusMenuClient *client, int parentId, QWidget *anchor,
                   const QPoint &rel)
    : ShellPopup(anchor, 240), m_client(client), m_parentId(parentId),
      m_topLevel(false)
{
    m_manageAsTopLevel = false;
    setAnchorOffset(rel);
    init();
}

void TrayMenu::init()
{
    connect(m_client, &DBusMenuClient::layout, this, &TrayMenu::onLayout);
    connect(m_client, &DBusMenuClient::updated, this, [this]() {
        if (isVisible())
            m_client->fetchLayout(m_parentId);
    });
    /* closing this menu closes any open submenus */
    connect(this, &ShellPopup::popupVisibleChanged, this, [this](bool v) {
        if (!v) {
            for (const QPointer<TrayMenu> &s : m_subs)
                if (s && s->isVisible())
                    s->closePopup();
        }
    });
}

void TrayMenu::openPopup()
{
    m_client->aboutToShow(m_parentId);
    m_client->fetchLayout(m_parentId);
    ShellPopup::openPopup();
}

void TrayMenu::dismiss()
{
    closePopup();
    emit dismissed();
}

int TrayMenu::popupHeight() const
{
    /* menuCol.implicitHeight + 8: rows + 1px spacing, 4px padding all round */
    int h = 0;
    for (const DBusMenuClient::Entry &e : m_entries)
        h += (e.separator ? 7 : 26) + 1;
    if (!m_entries.isEmpty())
        h -= 1;
    return h + 8;
}

void TrayMenu::onLayout(int parentId,
                        const QVector<DBusMenuClient::Entry> &entries)
{
    if (parentId != m_parentId)
        return;
    m_entries.clear();
    for (const DBusMenuClient::Entry &e : entries)
        if (e.visible)
            m_entries.push_back(e);
    rebuildRows();
}

void TrayMenu::rebuildRows()
{
    qDeleteAll(m_rows);
    m_rows.clear();

    relayout();

    /* Submenus are anchored to the row that opened them, and those rows were
     * just deleted. Re-anchor each to its replacement row below, or close it
     * if its entry is gone -- otherwise it would be positioned against a
     * dead anchor and land at the output's top-left corner. */
    QHash<int, QWidget *> rowForId;
    int y = 4;
    for (const DBusMenuClient::Entry &e : m_entries) {
        auto *row = new MenuRow(e, this);
        row->setGeometry(4, y, width() - 8, e.separator ? 7 : 26);
        y += (e.separator ? 7 : 26) + 1;
        row->onClick = [this](MenuRow *r, const DBusMenuClient::Entry &en) {
            if (en.hasChildren) {
                for (const QPointer<TrayMenu> &s : m_subs) {
                    if (s && s->m_parentId == en.id) {
                        s->setAnchor(r);
                        s->openPopup();
                        return;
                    }
                }
                auto *sub = new TrayMenu(m_client, en.id, r,
                                         QPoint(r->width() - 6, 0));
                connect(sub, &TrayMenu::dismissed, this, &TrayMenu::dismiss);
                m_subs.push_back(sub);
                sub->openPopup();
            } else {
                m_client->triggerEvent(en.id);
                dismiss();
            }
        };
        row->show();
        m_rows.push_back(row);
        rowForId.insert(e.id, row);
    }

    for (const QPointer<TrayMenu> &s : std::as_const(m_subs)) {
        if (!s)
            continue;
        if (QWidget *row = rowForId.value(s->m_parentId)) {
            s->setAnchor(row);
            if (s->isVisible())
                s->relayout();
        } else if (s->isVisible()) {
            s->closePopup();
        }
    }
}
