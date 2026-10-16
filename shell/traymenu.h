/* A themed context menu built from a com.canonical.dbusmenu
 * tree (instead of the native platform menu). Recurses for submenus. */
#ifndef FANSHELL_QS_TRAYMENU_H
#define FANSHELL_QS_TRAYMENU_H

#include <QDBusMessage>
#include <QImage>
#include <QPointer>

#include "popup.h"

/* Client for one com.canonical.dbusmenu object. */
class DBusMenuClient : public QObject {
    Q_OBJECT
public:
    struct Entry {
        int id = -1;
        QString label;
        bool enabled = true;
        bool visible = true;
        bool separator = false;
        int buttonType = 0; /* 0 none, 1 checkmark, 2 radio */
        bool checked = false;
        QString iconName;
        QImage iconData;
        bool hasChildren = false;
    };

    DBusMenuClient(const QString &service, const QString &path,
                   QObject *parent = nullptr);

    /* Fetch the children of `parentId`; result delivered via layout(). */
    void fetchLayout(int parentId);
    void aboutToShow(int parentId);
    void triggerEvent(int id);

signals:
    void layout(int parentId, const QVector<Entry> &entries);
    void updated();

private:
    QString m_service;
    QString m_path;
};

class TrayMenu : public ShellPopup {
    Q_OBJECT
public:
    /* Top-level menu (owns the client). */
    TrayMenu(const QString &service, const QString &path, QWidget *anchor);
    /* Submenu sharing the parent's client. */
    TrayMenu(DBusMenuClient *client, int parentId, QWidget *anchor,
             const QPoint &rel);

    void openPopup() override;

signals:
    void dismissed();

public slots:
    void dismiss();

protected:
    int popupHeight() const override;

private:
    void init();
    void onLayout(int parentId, const QVector<DBusMenuClient::Entry> &entries);
    void rebuildRows();

    DBusMenuClient *m_client;
    int m_parentId = 0;
    bool m_topLevel = true;
    QVector<DBusMenuClient::Entry> m_entries;
    QVector<QWidget *> m_rows;
    QVector<QPointer<TrayMenu>> m_subs;
};

#endif
