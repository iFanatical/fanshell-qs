/* Clipboard history -- tracks the CLIPBOARD selection only (text), not
 * PRIMARY. A pill that opens a pick-one-to-restore popup, closest in spirit
 * to the launcher's grid rather than the other widgets' stay-open control
 * panels.
 *
 * On X11 this is plain QClipboard: Qt hears about every change of selection
 * owner through XFixes, whichever window made the copy -- fanhypr-qs needs
 * `wl-paste --watch` only because Wayland withholds that from unfocused
 * clients. Restoring an entry makes the shell itself the selection owner,
 * which it stays for as long as it runs. */
#ifndef FANSHELL_QS_CLIPBOARD_H
#define FANSHELL_QS_CLIPBOARD_H

#include <QString>
#include <QVector>

#include "popup.h"
#include "procutil.h"
#include "widgets.h"

class ClipboardState : public QObject {
    Q_OBJECT
public:
    static ClipboardState *instance();

    static constexpr int maxHistory = 50;
    QVector<QString> history; /* most recent first */
    /* Always true on X11; kept so the widget code matches fanhypr-qs, where
     * it hides without wl-clipboard. */
    bool available = true;

    void selectEntry(int index);
    void removeEntry(int index);
    void clear();

signals:
    void changed();

private:
    explicit ClipboardState(QObject *parent = nullptr);
    void addEntry(const QString &text);
    void onClipboardChanged();
};

class ClipboardPopup;

class ClipboardWidget : public BarPill {
    Q_OBJECT
public:
    explicit ClipboardWidget(QWidget *parent = nullptr);

private:
    ClipboardPopup *m_popup;
};

class ClipboardPopup : public ContentPopup {
    Q_OBJECT
public:
    explicit ClipboardPopup(QWidget *anchor);

private:
    void rebuild();

    ShellButton *m_clearBtn;
    QWidget *m_listBox;
};

#endif
