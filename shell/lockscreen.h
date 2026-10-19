/* The lock screen, built into fanshell-qs-shell but run as a process of its
 * own, so a crash in the bar can never unlock the session:
 *
 *     fanshell-qs-shell lock
 *
 * It returns once the screen is locked (status 0, or at once if it already
 * is), 1 when the keyboard or pointer cannot be grabbed, and 2 when it cannot
 * lock here at all -- no X display, or no PAM service to check the password
 * with -- so scripts/fanshell-qs-lock can try another locker.
 *
 * Three processes, the way i3lock's fork works plus a watchdog:
 *
 *   the caller   takes the lock file, forks the supervisor, and exits once
 *                the locker reports the screen locked;
 *   supervisor   (comm "fanshell-lock") holds the lock file for as long as
 *                the session is locked. It never touches Qt. When the locker
 *                dies without the right password having been typed -- a
 *                crash, the OOM killer -- it starts a new one at once, so a
 *                bug can make the lock screen restart but not go away;
 *   locker       the Qt process: one window per output, the keyboard and
 *                pointer grabbed, the password checked through PAM.
 *
 * `pkill -x fanshell-lock` from another VT ends both, as killing i3lock
 * would.
 *
 * The screen shows the clock over the blurred wallpaper until a key or click,
 * then the user, a password field and messages on the primary output. Media
 * controls, the battery, the weather and the uptime are shown throughout;
 * volume and brightness keys are passed on to the shell. After lock.conf's
 * display_off seconds without input the displays are powered down (DPMS),
 * whatever programs asked of the screen saver.
 *
 * The PAM service is /etc/pam.d/fanshell-qs-lock (tools/install-shell
 * installs it), else i3lock's, so a machine that could unlock i3lock can
 * unlock this. With neither, it refuses to lock rather than lock for good. */
#ifndef FANSHELL_QS_LOCKSCREEN_H
#define FANSHELL_QS_LOCKSCREEN_H

#include <QString>

namespace LockScreen {

/* `fanshell-qs-shell lock`; returns the process exit status. */
int run(int argc, char **argv);

/* $XDG_RUNTIME_DIR/fanshell-qs-lock-<display>.lock, held while locked. */
QString lockFilePath();
/* Whether a lock screen holds that file now. */
bool isLocked();

} /* namespace LockScreen */

#endif
