/* Clipboard history -- tracks the CLIPBOARD selection only (text), not
 * PRIMARY. A pill that opens a pick-one-to-restore popup, closest in spirit
 * to the launcher's grid rather than the other widgets' stay-open control
 * panels.
 *
 * On X11 this is plain QClipboard: Qt hears about every change of selection
 * owner through XFixes, whichever window made the copy, and restoring an
 * entry makes the shell itself the selection owner for as long as it runs.
 * Wayland withholds the selection from unfocused clients, so there it is
 * `wl-paste --watch` (wl-clipboard), and copies go through wl-copy, which
 * forks a server of its own to hold the selection. */
#ifndef FANSHELL_QS_CLIPBOARD_H
#define FANSHELL_QS_CLIPBOARD_H

#include <QString>
#include <QVector>

#include "popup.h"
#include "procutil.h"
#include "widgets.h"

class BlockWatchProcess;
class QImage;

/* Put text or an image (as PNG) on the CLIPBOARD selection: wl-copy on
 * Wayland, QClipboard on X11. */
namespace Clipboard {
void copyText(const QString &text);
void copyImage(const QImage &image);
} /* namespace Clipboard */

class ClipboardState : public QObject {
    Q_OBJECT
public:
    static ClipboardState *instance();

    static constexpr int maxHistory = 50;
    QVector<QString> history; /* most recent first */
    /* False on Wayland without wl-clipboard: the widget hides. */
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

    BlockWatchProcess *m_watch = nullptr;
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
