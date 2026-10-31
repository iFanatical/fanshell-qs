# fanshell-qs

The hand-rolled C++/Qt bar and desktop shell for X11 and Hyprland, shared
by three sessions:

- [fanoxwm-qs](https://github.com/iFanatical/fanoxwm-qs): oxwm, with the
  IPC patch the shell talks to, and its Lua config.
- [fanqtile-qs](https://github.com/iFanatical/fanqtile-qs): qtile and its
  config.
- [fanhypr-qs](https://github.com/iFanatical/fanhypr-qs): Hyprland and its
  Lua config (see [Hyprland](#hyprland)).

One binary (`fanshell-qs-shell`) draws the panel, the popups, the
StatusNotifierItem and XEmbed tray, notifications, OSDs, the launcher, the
wallpaper picker and the Control Panel, in place of the window manager's
own bar. The same build runs under either window manager, so a feature
added here is in both sessions at once.

Started from fanoxwm-qs `5ad640b`, which had the shell, scripts and session
in one repo, and from fanhypr-qs `9ea1f08` before that.

## Layout

```text
~/projects/window-managers/
├── fanshell-qs/             this repo: the shell, used by both sessions
│   ├── shell/                the Qt shell
│   ├── scripts/fanshell-qs-* helper scripts the widgets exec
│   ├── tools/                link-config, install-shell and the RAPL installer
│   └── config/               sxhkdrc, picom.conf, alacritty.toml, websites.json, output defaults, PAM service, portals.conf, the systemd session target
├── fanoxwm-qs/               the oxwm session
├── fanqtile-qs/              the qtile session
└── oxwm/                     upstream oxwm, read by fanoxwm-qs's build
```

## New machine

Everything from a fresh install to a running session, on Fedora. The
session repos' `install.sh` does all the building, installing and linking;
what it can't do is install packages, so that comes first.

**1. Packages.** The install scripts check for these and stop with a hint
if one is missing, but never install them. The first three lines (the
shell and oxwm build) and `xorg…` through `sxhkd` are required; the rest
are the optional features listed under [Requirements](#requirements).

```sh
sudo dnf install git gcc gcc-c++ make pkgconf-pkg-config \
    qt6-qtbase-devel libxcb-devel libX11-devel libXcomposite-devel \
    libXdamage-devel libXfixes-devel libxkbfile-devel pulseaudio-libs-devel pam-devel \
    zig libXft-devel libXinerama-devel fontconfig-devel \
    xorg-x11-server-Xorg xorg-x11-xinit picom xrandr sxhkd \
    dbus-tools xdg-desktop-portal xdg-desktop-portal-gtk glib2 \
    jq xdotool xprop NetworkManager bluez brightnessctl \
    ddcutil power-profiles-daemon wireguard-tools net-snmp-utils curl \
    pipewire-utils alacritty xdg-utils xsetroot xsettingsd \
    mate-polkit
```

oxwm needs Zig 0.16.0, which is what Fedora 44 ships; an older release's
`zig` is too old, and `make oxwm` says so.

**2. Font.** JetBrainsMono Nerd Font from
[nerdfonts.com](https://www.nerdfonts.com) into `~/.local/share/fonts`,
then `fc-cache -f`. Fedora doesn't package it, and the bar's icons come
from it.

**3. Clone side by side.** The install scripts expect these names next to
each other (or `$FANSHELL_QS_DIR` and `$OXWM_SOURCE_DIR`):

```sh
mkdir -p ~/projects/window-managers && cd ~/projects/window-managers
git clone git@github.com:iFanatical/fanshell-qs.git
git clone git@github.com:iFanatical/fanoxwm-qs.git
git clone git@github.com:iFanatical/fanqtile-qs.git     # for qtile, later
git clone https://github.com/tonybanters/oxwm.git
git -C oxwm checkout 472c767     # the commit hooks.patch was last built against
```

The `git@github.com:` URLs need an SSH key on GitHub. oxwm is pinned
because upstream can move code next to the patched lines, which stops
`make oxwm` with "hooks.patch no longer applies"; to move to a newer oxwm,
check out `main` and refresh `oxwm-ipc/hooks.patch` in fanoxwm-qs if needed.

**4. Build and install.** As yourself, not root; it asks for sudo only to
install:

```sh
cd ~/projects/window-managers/fanoxwm-qs
./install.sh --dry-run    # lists every step first
./install.sh
```

It builds patched oxwm (the first build downloads Lua through Zig, so it
needs the network) and `oxwmctl`, installs them and the oxwm scripts into
`/usr/local/bin`, links `~/.xinitrc`, `~/.X_envars` and `~/.config/oxwm`,
then builds, installs and links the shell. A real file in the way of a link
is moved to `NAME.bak-DATE`, never overwritten. If a dependency the shell
or its session needs is missing, it lists them, asks whether to install them
(`dnf`, `emerge` or `pacman`, through sudo), and stops if you say no.

Optional, from this repo's `tools/`: `sudo ./install-rapl-udev.sh` for
CPU wattage in the power pill.

**5. Log in.** `startx` from a text console starts oxwm, which starts the
bar.

**Not in these repos**, but the session expects them:

- wallpapers in `~/Pictures/wallpapers` (or `wallpaper.dir` in
  [`shell.conf`](#configuration));
- the Tokyo-Night GTK theme and Tela-circle icons that `fanshell-qs-session` sets
  through `gsettings`;
- the helper scripts `config/sxhkdrc` calls from `$SCRIPTS`
  (`~/.local/bin`): `screenshot.sh` (Print, Ctrl+Alt+Tab) needs maim,
  xclip, xdotool and notify-send (Fedora `maim xclip xdotool libnotify`;
  Gentoo `media-gfx/maim x11-misc/xclip x11-misc/xdotool x11-libs/libnotify`),
  and `xcolorpick.sh` (Alt+C) needs xcolor, which neither distribution packages
  (`cargo install xcolor`), plus xclip and notify-send;
- the rest of your dotfiles: `~/.zshrc`, nvim;
- `~/.config/fanshell-qs`, the shell's saved settings (wallpaper,
  vibrance, display layouts, screen power, lock and night light). It starts empty with
  defaults; copy it from another machine to keep the same choices.

**Switching to qtile** later: `sudo dnf install qtile` (Gentoo:
`sudo emerge x11-wm/qtile`), then `./install.sh` in fanqtile-qs, which
points `~/.xinitrc` at qtile and reinstalls the shell; `./install.sh` in
fanoxwm-qs switches back. fanqtile-qs's README lists what else the qtile
side uses.

**On Gentoo**, the steps are the same with the `emerge` lines under
[Installing](#installing) and in fanoxwm-qs's README.

## Window managers

The shell talks to the window manager only through `WmState`
(`shell/wmstate.h`): the outputs, each one's workspaces, layout and focused
title, and a few actions. A backend fills it in:

| backend | window manager | status |
|---|---|---|
| `oxwmstate.*` | the patched oxwm from fanoxwm-qs, over its IPC sockets | complete |
| `qtilestate.*`, `qtileipc.*` | qtile, over its own IPC socket; hooks in its config send `wm changed` (fanqtile-qs does this) | complete |
| `hyprstate.*`, `hypripc.*` | Hyprland, over its two IPC sockets (Wayland) | complete; see [Hyprland](#hyprland) |

At startup the shell picks the backend for the window manager that is
running: `$FANSHELL_QS_WM` if set, else Hyprland on Wayland (or with
`$HYPRLAND_INSTANCE_SIGNATURE` set), else the name the window manager gives
EWMH clients (`_NET_WM_NAME` on its `_NET_SUPPORTING_WM_CHECK` window), else
`$XDG_CURRENT_DESKTOP`, else qtile when its IPC socket for this display
exists (qtile's stock config calls itself `LG3D`), else oxwm. A few other
things follow the window manager too: logging out uses its own quit, and the
Fanos menu's configuration entries open its config.

## Requirements

Package names are Fedora / Gentoo. A dash means the distribution doesn't
package it.

### Required

Without these, the build fails or the session doesn't come up. The window
manager itself, and what it needs to build, is listed in its own repo.

| What | Fedora | Gentoo | For |
|---|---|---|---|
| C/C++ toolchain | `gcc` `gcc-c++` `make` `pkgconf-pkg-config` | `sys-devel/gcc` `dev-build/make` `dev-util/pkgconf` | building the shell |
| Qt 6 base | `qt6-qtbase-devel` | `dev-qt/qtbase` (USE `dbus gui network widgets X`) | the shell: widgets, D-Bus, sockets, and the xcb platform plugin |
| libxcb (with RandR, DPMS, the screen saver extension, XKB and XInput) | `libxcb-devel` | `x11-libs/libxcb` | the panel's dock surface, the Displays popup, digital vibrance, night light, Screen power, the lock screen and lock on idle, the Keyboard settings |
| PAM | `pam-devel` | `sys-libs/pam` | checking the password on the lock screen; `tools/install-shell` installs its service, `/etc/pam.d/fanshell-qs-lock` |
| libxkbfile | `libxkbfile-devel` | `x11-libs/libxkbfile` | the Keyboard settings' Caps Lock and Compose options, read from the XKB rules as setxkbmap does (it is also what X's own `xkbcomp` uses, so it is usually there already) |
| X11, XComposite, XDamage, XFixes | `libX11-devel` `libXcomposite-devel` `libXdamage-devel` `libXfixes-devel` | `x11-libs/libX11` `x11-libs/libXcomposite` `x11-libs/libXdamage` `x11-libs/libXfixes` | the XEmbed tray |
| libpulse | `pulseaudio-libs-devel` | `media-libs/libpulse` | volume and microphone control; works against pipewire-pulse |
| An X server and xinit | `xorg-x11-server-Xorg` `xorg-x11-xinit` | `x11-base/xorg-server` `x11-apps/xinit` | running the session from `~/.xinitrc` with `startx` (XLibre works too) |
| A D-Bus session bus | `dbus` `dbus-tools` | `sys-apps/dbus` | notifications, the StatusNotifierItem tray, media controls; `fanshell-qs-session` uses `/run/user/UID/bus` when systemd or an OpenRC user service provides it, and otherwise starts one with `dbus-run-session` (Fedora `dbus-daemon`) |
| xdg-desktop-portal and its GTK backend | `xdg-desktop-portal` `xdg-desktop-portal-gtk` | `sys-apps/xdg-desktop-portal` `sys-apps/xdg-desktop-portal-gtk` | file pickers and the colour scheme for GTK, Electron and Brave; `config/xdg-desktop-portal/portals.conf` picks the GTK backend |
| glib2 (`gsettings`) | `glib2` | `dev-libs/glib` | GTK theme, icons, font and colour scheme, which the portal passes on |
| picom, or another compositor | `picom` | `x11-misc/picom` | translucent panel and toasts, and the popup scrim, which is otherwise drawn opaque |
| xrandr | `xrandr` | `x11-apps/xrandr` | applying output layouts, at login and from the Displays popup |
| sxhkd | `sxhkd` | `x11-misc/sxhkd` | every program, popup and media hotkey in `config/sxhkdrc`; the window manager only binds window management |
| A Nerd Font | — | — | `JetBrainsMono Nerd Font Propo` for the bar's icons, from [nerdfonts.com](https://www.nerdfonts.com) |

### Optional

Each one enables a single feature. Anything missing just leaves that feature
out or empty.

| What | Fedora | Gentoo | For |
|---|---|---|---|
| NetworkManager | `NetworkManager` | `net-misc/networkmanager` | the network pill and Wi-Fi list (`nmcli`) |
| BlueZ | `bluez` | `net-wireless/bluez` | the Bluetooth popup (`bluetoothctl`) |
| brightnessctl | `brightnessctl` | `app-misc/brightnessctl` | laptop screen and keyboard backlight (the brightness keys, and the System dropdown's keyboard row) |
| ddcutil | `ddcutil` | `app-misc/ddcutil` | external monitor brightness over DDC/CI (needs the `i2c-dev` module and access to `/dev/i2c-*`) |
| power-profiles-daemon | `power-profiles-daemon` | `sys-power/power-profiles-daemon` | the System dropdown's power-profile selector |
| wireguard-tools | `wireguard-tools` | `net-vpn/wireguard-tools` | the VPN pill; `wg-quick` runs through passwordless `sudo -n` |
| net-snmp | `net-snmp-utils` | `net-analyzer/net-snmp` | UPS wattage in the power pill, when `ups.host` is set (see [UPS](#ups)) |
| curl | `curl` | `net-misc/curl` | the weather pill and the lock screen's weather (wttr.in) |
| pw-play or paplay | `pipewire-utils` or `pulseaudio-utils` | `media-video/pipewire` | the optional volume-change sound |
| A terminal | `alacritty` or `xterm` | `x11-terms/alacritty` or `x11-terms/xterm` | `Terminal=true` launcher entries and the menu's actions (`$TERMINAL`, then alacritty for the menu, xterm for the launcher) |
| xdg-utils | `xdg-utils` | `x11-misc/xdg-utils` | the menu opening URLs and files |

### Session extras

`fanshell-qs-session` (see [Session](#session)) also starts or calls
these. None of them are needed by the shell: a daemon that is not installed
is skipped, and the rest can be dropped from `scripts/fanshell-qs-session`
instead of installed.

| What | Fedora | Gentoo | For |
|---|---|---|---|
| xsetroot | `xsetroot` | `x11-apps/xsetroot` | the root window's arrow cursor |
| xsettingsd | `xsettingsd` | `x11-misc/xsettingsd` | GTK/Qt theme and font settings for X apps |
| A polkit agent | `mate-polkit` | `mate-extra/mate-polkit` | password prompts for privileged actions |
| gpu-screen-recorder-ui | — | — | `gsr-ui launch-daemon`, screen recording |

### Installing

Required:

```sh
# Fedora
sudo dnf install gcc gcc-c++ make pkgconf-pkg-config \
    qt6-qtbase-devel libxcb-devel libX11-devel libXcomposite-devel \
    libXdamage-devel libXfixes-devel libxkbfile-devel pulseaudio-libs-devel pam-devel \
    xorg-x11-server-Xorg xorg-x11-xinit picom xrandr sxhkd \
    dbus-tools xdg-desktop-portal xdg-desktop-portal-gtk glib2

# Gentoo
sudo emerge dev-qt/qtbase x11-libs/libxcb x11-libs/libX11 \
    x11-libs/libXcomposite \
    x11-libs/libXdamage x11-libs/libXfixes x11-libs/libxkbfile media-libs/libpulse sys-libs/pam \
    x11-base/xorg-server x11-apps/xinit x11-misc/picom x11-apps/xrandr \
    x11-misc/sxhkd sys-apps/dbus sys-apps/xdg-desktop-portal \
    sys-apps/xdg-desktop-portal-gtk dev-libs/glib
```

Optional, everything above:

```sh
# Fedora
sudo dnf install NetworkManager bluez \
    brightnessctl ddcutil power-profiles-daemon wireguard-tools \
    net-snmp-utils curl pipewire-utils alacritty xdg-utils \
    xsetroot xsettingsd mate-polkit

# Gentoo
sudo emerge net-misc/networkmanager net-wireless/bluez \
    app-misc/brightnessctl app-misc/ddcutil sys-power/power-profiles-daemon \
    net-vpn/wireguard-tools net-analyzer/net-snmp net-misc/curl \
    media-video/pipewire x11-terms/alacritty \
    x11-misc/xdg-utils x11-apps/xsetroot \
    x11-misc/xsettingsd mate-extra/mate-polkit
```

## Build

```sh
make                       # shell/fanshell-qs-shell
sudo make install          # shell and helper scripts into /usr/local/bin
make link-config           # link this repo's config files into $HOME
```

Or `tools/install-shell`, which also checks the build and session
dependencies (offering to install what is missing, and stopping without
them; the list is `need_shell` in `tools/lib.sh`, which the session repos'
`install.sh` check too, before they build anything), moves a
running session onto the new build (restarting the bar and reloading sxhkd)
and removes the files fanoxwm-qs installed before the split. The session
repos' `install.sh` run it as their last step.

Then set up a session from fanoxwm-qs or fanqtile-qs: each starts the shell
from its window manager's config and links `~/.xinitrc`, `~/.X_envars` and
the window manager's own config.

### Session

Everything the sessions share lives here, so the two session repos hold only
what is their window manager's own and cannot drift apart:

- **`fanshell-qs-session WM [ARGS...]`** is the whole X session around the
  window manager: the environment (`PATH`, `SCRIPTS`, the D-Bus address,
  XDG user dirs, `XDG_SESSION_TYPE`, `_JAVA_AWT_WM_NONREPARENTING`), then
  `~/.X_envars` (the session repo's, for what is its window manager's own,
  such as `XDG_CURRENT_DESKTOP`), the saved output layout, the cursor, the
  [session daemons](#session-extras) and the GTK theme. It hands `DISPLAY`,
  `XAUTHORITY` and `XDG_CURRENT_DESKTOP` to the bus, for the programs it
  starts on demand (the portals), and under systemd starts
  `fanshell-qs-session.target`, which brings up `graphical-session.target`
  so the portal can start under `startx`. Then it execs `WM ARGS...`.
  Each session repo's `xinitrc` is that one line, `exec fanshell-qs-session
  qtile start` or `... oxwm`, falling back to the bare window manager when
  fanshell-qs is not installed.
- **`tools/lib.sh`** is what the install and link scripts share: `--dry-run`
  handling, `step`/`run`/`die`, and `link()`, which makes the `$HOME`
  symlinks (repointing one that points at the other session repo, moving a
  real file aside to `NAME.bak-DATE`). The session repos' `install.sh` and
  `tools/link-config` source it from the fanshell-qs checkout beside them
  (or `$FANSHELL_QS_DIR`).
- **`config/sxhkdrc`**, the bar's keys, under either window manager.

What stays in each session repo is its window manager's: its config (key
bindings, rules, layouts, workspaces), how it starts the bar, `X_envars`, and
the one line of `xinitrc` naming it.

To restart the shell after a rebuild or an environment change, run
`fanshell-qs-restart`.

### Config files

`make link-config` (or `tools/link-config`) links these into `$HOME`,
pointing at wherever this repo is checked out. A real file in the way is
moved to `NAME.bak-DATE` first, and `--dry-run` shows what would change:

| file | linked from |
|---|---|
| `config/sxhkdrc` | `~/.config/sxhkd/sxhkdrc` (program, popup and media keys; the same under both window managers) |
| `config/picom.conf` | `~/.config/picom/picom.conf` |
| `config/alacritty.toml` | `~/.config/alacritty/alacritty.toml` (the terminal sxhkd and the window managers open) |
| `config/websites.json` | `~/.config/fanshell-qs/websites.json` (the menu's Websites page) |
| `config/outputs.defaults.conf` | `~/.config/fanshell-qs/outputs.defaults.conf` (fallback output layouts) |
| `config/mouse.defaults.conf` | `~/.config/fanshell-qs/mouse.defaults.conf` (pointer speeds for every machine, by part of a device name) |
| `config/xdg-desktop-portal/portals.conf` | `~/.config/xdg-desktop-portal/portals.conf` (the GTK portal backend, for every session) |
| `config/systemd/fanshell-qs-session.target` | `~/.config/systemd/user/fanshell-qs-session.target` (systemd hosts only) |

The tray and toasts live on X's primary output (`xrandr --primary`). The
launcher, wallpaper picker and Control Panel open on the output the window
manager has focused.

### What the shell remembers

Everything the shell persists lives in `~/.config/fanshell-qs/` (or
`$XDG_CONFIG_HOME/fanshell-qs/`), never under `~/.cache`, so clearing the
cache loses nothing you chose:

| file | |
|---|---|
| `wallpaper.conf` | the picker's wallpaper and mode for every output (`path`, `mode`) and for any output that differs (`path.<output>`, `mode.<output>`); drawn when the shell starts and when the outputs change |
| `vibrance.conf` | digital vibrance per output; re-applied at start and when an output connects |
| `outputs.conf` | output layouts, one per set of connected outputs; applied at login (`fanshell-qs-session`) and on hotplug |
| `dpms.conf` | screen blanking and DPMS timeouts; applied at startup |
| `lock.conf` | lock on idle, before sleep, the fullscreen and audio pauses, and display off when locked |
| `nightlight.conf` | night light on or off, its temperatures, and the location it was last found at |
| `keyboard.conf` | key repeat, Caps Lock and Compose keys, Num Lock at login, lock OSD, backlight timeout |
| `mouse.conf` | pointer settings per device, by name |
| `websites.json` | linked from the repo (above) |
| `sounds/` | optional `audio-volume-change.*` for volume feedback |

The shell draws the wallpaper itself (no feh) and sets it as the root
window's background, published in `_XROOTPMAP_ID` for picom. The picker's
"Apply to" drop-down sets it for every output or for one. If
`wallpaper.conf` does not exist yet, the path in an old `~/.fehbg` is imported
once. After that, `~/.fehbg` can be deleted. Runtime-only state (IPC sockets,
scratchpad window ids, the `maximize` layout memo) stays in
`$XDG_RUNTIME_DIR`: it means nothing after a reboot.

### Control Panel

The System dropdown is for status and quick actions: CPU, memory,
temperature and disk, weather, Bluetooth, clipboard, VPN, Keep awake and
Game mode, the power profile, the keyboard backlight (on a laptop that has
one), audio (output, input, each program's volume, the output device),
and one row of icons for Lock, Sleep, Log out, Reboot and Power off (named
in the caption below the row as you point at them; the last three ask for a
second click). Settings live in the **Control Panel**, opened from the
dropdown's header (or `ipc call controlpanel toggle`). It is a grid of
tiles, each showing a line of live status and opening its own window:
Displays, Screen power, Screen lock, Keyboard, Mouse & touchpad,
Wallpaper and Night light. A new settings window
becomes a tile in `shell/controlpanel.cpp`.

### Displays

`fanshell-qs-shell ipc call outputs toggle` (or Control Panel → Displays)
opens the Displays popup. Drag the outputs to arrange them: a dropped
output snaps flush to its neighbours' edges and never overlaps them. For the
selected output, set it on or off, primary, its resolution, refresh rate and
rotation, and its digital vibrance.

**Apply** tries the layout. Choose **Keep** within 15 seconds, or it reverts
by itself, so a mode the monitor can't show can't strand you. Keeping saves
it to `outputs.conf`, under the set of outputs connected now
(`DisplayPort-0+DisplayPort-1`, `eDP-1`, ...). Each machine, and the laptop
docked or not, finds its own layout, with no hostname or battery test.
`fanshell-qs-session` runs `fanshell-qs-outputs apply` at login. When the connected set
changes, the shell applies the layout saved for the new set. A set with no
layout in `outputs.conf` falls back to `outputs.defaults.conf`, linked from
the repo, which holds the desktop and laptop layouts `monitor-config.sh` used
to set.

`scripts/fanshell-qs-outputs` is the only thing that turns a layout into an
xrandr command (its header documents the file format). After xrandr it runs
`oxwmctl rescanmonitors` when oxwm is running: oxwm, like dwm, only re-reads
the monitor layout when the root window changes size, so swapping two equal
monitors would otherwise leave it tiling into the old geometry. qtile
re-reads the screens after any RandR change by itself.

**Screen power** in the Control Panel (or `ipc call dpms toggle`) sets how
long the screen may sit idle before it blanks, and DPMS power saving,
on or off, with its standby, suspend and off timeouts. Changes apply at once
and are saved to `dpms.conf`, which the shell applies at startup; without
one, blanking and DPMS are both off, as `.xinitrc` used to ask of `xset`.
**Turn off now** (or `ipc call dpms off`) powers the displays down until the
next input, even with DPMS otherwise off. All of it goes over xcb, so `xset`
need not be installed.

Vibrance, 0% greyscale to 200%, with 100% normal at the slider's centre,
applies at once and is saved separately. Click the percentage to reset it. It
works like [libvibrant](https://github.com/libvibrant/libvibrant)'s CTM
backend, setting the RandR `CTM` property (the kernel's colour
transformation matrix) directly over xcb, so there is nothing extra to
install. Outputs whose driver has no `CTM` property, notably NVIDIA's
proprietary one, get no slider.

### Night light

**Night light** in the Control Panel (or `ipc call nightlight toggle`)
warms the screen's colours after sunset, as redshift or f.lux do. It follows
the sun where the machine is: full day while the sun is 3° or more above
the horizon, full night once it is 6° below, and in between with the sun's
height, about 40 minutes at mid latitudes. Night defaults to 4500 K and day
to 6500 K (normal colours); both are drop-downs. The location is looked up
from your IP address at wttr.in once per session, as the weather is, and
the last one found is kept for when the network is not up. Set
`location=manual` with `latitude=` and `longitude=` in `nightlight.conf` to
fix it instead (a VPN would otherwise move you). Until a location has been
found, it warms from 19:30 to 20:30 and returns from 06:30 to 07:30.

`ipc call nightlight enable`, `disable` and `toggle-enabled` switch it from
a key binding. It sets the outputs' RandR gamma ramps, so it works on
NVIDIA's proprietary driver too, and stacks with digital vibrance (the CTM,
applied before the ramps). They are written again after any RandR change,
since a modeset can reset them, and set back to normal when the shell
quits. Don't run redshift or gammastep alongside it: they would fight over
the ramps.

### Screen lock

The Lock button, and every automatic lock, run `fanshell-qs-lock`, which
runs the shell's own lock screen, `fanshell-qs-shell lock`. It shows the
time and date over the blurred wallpaper, the weather (bottom left), the
uptime (bottom right), the battery on a laptop (top right), and the playing
music with its cover and previous/play/next buttons. A key or click brings
up your name and picture (`~/.face`, or AccountsService's), and a password
field with a blinking caret; that first key is not typed into it, nor are
its repeats while held.
Escape goes back to the clock, as does half a minute without input.
Caps Lock and a wrong password are shown under the field. The volume,
brightness and media keys keep working, but no notification or OSD shows
while locked.

The password is checked by PAM, through `/etc/pam.d/fanshell-qs-lock`
(`config/pam.d/fanshell-qs-lock`, which `tools/install-shell` installs; it
defers to `login`), or i3lock's service when that is missing. With neither
it refuses to lock, and the script falls back to i3lock, xsecurelock, slock,
xscreensaver and then `loginctl lock-session`.

The lock screen is the shell's binary but a process of its own, so the bar
crashing never unlocks the session. A small supervisor (`fanshell-lock` in
`ps`) holds a lock file in `$XDG_RUNTIME_DIR` while locked, and starts the
lock screen again at once if it ever dies without the password having been
typed. `pkill -x fanshell-lock` from another VT ends it, as killing i3lock
would.

**Screen lock** in the Control Panel (or `ipc call lock toggle`) sets when
the screen locks by itself:

- **Lock after**: idle time before the screen locks, or Never. Idle is time
  without keyboard or mouse input, read from the X screen saver extension,
  so it is separate from Screen power's blanking.
- **Before sleep**: lock as the machine suspends. The shell holds a logind
  `delay` inhibitor and lets go once the screen is locked, so it is already
  locked on wake (see `systemd-inhibit --list`).
- **Pause for fullscreen**: don't lock while the focused window is
  fullscreen, such as a video or a game. It locks once that ends if you are
  still away.
- **Pause while audio plays**: don't lock while a program is playing
  sound: music, a call, a video in a window. Streams no program owns
  (loopbacks and virtual devices, which are always open) don't count, but
  a game with its sound on does. The window shows what is holding the
  lock off at the moment.
- **Display off when locked**: how long the lock screen stays lit without
  input before it powers the displays down (DPMS), 5 minutes by default.
  It counts from the last key or pointer move on the lock screen, and
  applies even while a program has asked the screen not to blank.

Screen power's blanking and DPMS timers are the X server's own and count
from the last input as well, whether or not the screen is locked. Whichever
is set shorter acts first: if the screen blanks before the lock time, the
lock still comes on behind the dark screen. The pauses above only hold off
the lock; with music playing, the screen still blanks on Screen power's
schedule.

Programs can also ask for it themselves: the shell answers
`org.freedesktop.ScreenSaver` on the session bus (`Inhibit`/`UnInhibit`),
which browsers playing video, video players and calls use. While one
holds such a request the screen neither locks nor blanks (the X screen
saver and DPMS are suspended too); a program that exits without letting go
is let go of. Without this nothing in these sessions answered those
requests.

`ipc call lock now` locks straight away. Settings are saved to `lock.conf`;
without one, the screen locks after 10 minutes, before sleep, and pauses for
fullscreen and while audio plays, and the lock screen turns the displays
off after 5 minutes.

### Keyboard

**Keyboard** in the Control Panel (or `ipc call keyboard toggle`):

- **Repeat delay** and **Repeat rate**: how long a held key waits before
  repeating, and how fast it repeats then (default 200 ms, 35 per second,
  what `xset r rate 200 35` set before). Set through XKB, so xset is not
  needed.
- **Caps Lock key**: Caps Lock, Escape, Ctrl, swapped with Ctrl, or
  nothing. **Compose key**: none, Right Alt, Menu or Right Ctrl; Compose
  then two keys types one character (`'` `e` is é). These are the XKB
  options `setxkbmap -option` sets (`caps:escape`, `compose:ralt`, ...),
  applied by the shell itself through libxkbfile, so setxkbmap is not
  needed either. Options it does not manage, from `xorg.conf` say, are
  kept.
- **Num Lock at login**, on by default: the shell turns Num Lock on when it
  first starts in an X session, with the same XKB call numlockx makes. It
  is done once per session (a property on the root window marks it), so
  restarting the shell leaves Num Lock as you last set it.
- **Show lock changes**: the OSD shows Caps Lock and Num Lock going on and
  off, for keyboards without indicator lights.
- **Backlight off after** (laptops with a keyboard backlight): switch the
  backlight off after a time without keyboard or mouse input, and back to
  its level at the next one. A level set while it is off (Fn+Space, the
  System dropdown) is kept.

A keyboard plugged in later starts with the X server's defaults, so the
shell applies the repeat rate and the options again when one is added.
Saved to `keyboard.conf`.

### Mouse & touchpad

**Mouse & touchpad** in the Control Panel (or `ipc call mouse toggle`)
sets, for the device picked at the top:

- **Speed**, -1 to 1 (0 is the driver's default), and **Acceleration**:
  adaptive, or flat for none, where the same hand movement always moves
  the pointer the same distance.
- **Natural scrolling** and **Left-handed** buttons.
- On a touchpad, **Tap to click**, **Disable while typing** and
  **Scrolling** with two fingers or along the edge.

Only what the device supports is shown, and only devices on the libinput
X driver (the default) are listed; one on evdev is set in `xorg.conf`
instead. These are the driver's XInput properties, set over xcb as
`xinput set-prop` would, so xinput is not needed. **Reset device** puts a
device back to the driver's defaults.

Settings follow a device by name, so they come back when it is plugged in
again: the popup saves them to `mouse.conf` by exact name, and
`mouse.defaults.conf`, linked from `config/`, sets them on every machine by
any part of a name, ignoring case (`speed:g pro=-0.5`). The popup's choice
wins over the defaults file.

### Status pills

These show on the bar only while they have something to say:

- **Disk**: `/` or `/home` at `disk.warn` percent full or more. The System
  dropdown's Disk tile shows the fuller of the two either way.
- **Keep awake / game mode**: a coffee cup and a gamepad while either is on;
  click turns them off.
- **Network speed**: download and upload rates in the network pill, over the
  physical interfaces (VPN tunnels and bridges are left out, since their
  traffic is counted again on the link). `network.speed=off` hides them.

### Keep awake and game mode

**Keep awake** (System dropdown, or `ipc call keepawake toggle|on|off`)
stops the screen blanking and locking until it is turned off, as a program
playing video can ask to. It is not remembered across restarts.

**Game mode** (System dropdown, or `ipc call gamemode toggle|on|off`) turns
on do not disturb, switches the power profile to Performance where there is
one, holds the night light (each gamma write stalls the X server for a few
frames) and keeps the screen from blanking or locking, since a controller
is not X input. Turning it off puts back the do-not-disturb setting and
power profile it found, and the night light catches up. With
`gamemode.auto=fullscreen` it turns itself on while the focused window is
fullscreen and off afterwards; turned off by hand meanwhile, it stays off
until that window stops being fullscreen.

### Colour picker and screenshots

Both work on a frozen copy of the screen as picom shows it, taken at the key
press, so nothing extra (maim, xclip, a picker) is needed.

- `ipc call picker pick` (Alt+C): a loupe follows the pointer; a click copies
  the pixel's colour as `#rrggbb` to the clipboard. Escape or right-click
  cancels.
- `ipc call screenshot region` (Print): drag a rectangle, across outputs if
  need be; a click without dragging takes the whole output. Escape or
  right-click cancels.
- `ipc call screenshot output` (Shift+Print): the output under the pointer.
- `ipc call screenshot screen` (Ctrl+Print): every output.

Screenshots are saved as PNGs in `screenshot.dir` (default
`~/Pictures/screenshots`), copied to the clipboard, and announced with a
thumbnail.

## Hyprland

The shell picks its platform at run time. Under Hyprland it is a Wayland
client, and the build includes that side when layer-shell-qt and
wayland-client are found (`WAYLAND=0` leaves it out, `WAYLAND=1` makes it
required; see the top of `shell/Makefile`). Every feature works there; what
does it differs:

| feature | X11 | Hyprland |
|---|---|---|
| panel, popups, toasts (`surface.*`) | EWMH docks, override-redirect windows | wlr-layer-shell surfaces (LayerShellQt) |
| window manager (`wmstate.*`) | `oxwmstate`, `qtilestate` | `hyprstate`: a fixed block of workspaces per output from the workspace rules, and the submap pill |
| clipboard | `QClipboard` | `wl-paste --watch`, `wl-copy` |
| wallpaper | the root window's pixmap | a background-layer surface per output |
| Displays | RandR, xrandr | `monitors all`, monitor rules (`fanshell-qs-outputs`); scale and adaptive sync, no primary or TearFree |
| vibrance, night light | RandR CTM, gamma ramps | one screen shader (`screenshader.*`), as hyprvibrance does |
| lock screen | X windows, grabs | ext-session-lock-v1 (`sessionlock.*`) |
| idle, screen power | X screen saver, DPMS | ext-idle-notify-v1, wlr-output-power-management (`wlclient.*`) |
| Keyboard, Mouse & touchpad | XKB, XInput properties | `input:` and `hl.device` options |
| screenshots, picker | the root window | grim |

Config changes go through `eval hl.config{...}` under Hyprland 0.56's Lua
config, and `keyword` under hyprlang (`hypripc.*` probes which). Hyprland
puts its own config back on a reload, and the shell re-applies its output
layout, input settings and shader then. Not tested on hardware yet: the
Lua forms `hl.monitor{disabled = true}`, `vrr` and `hl.device` merging
with a device's rules in `hyprland.lua`.

## Configuration## Configuration

Everything visual lives in `shell/theme.h`. Settings live in
`~/.config/fanshell-qs/shell.conf`, a hand-edited `KEY=VALUE` file (`#`
comments; a value starting with `~/` is under your home). It belongs to the
shell, so it is the same under oxwm and qtile, and the session repos set
none of it. Every key is optional; a missing file means all defaults. The
shell reads it at startup (restart it with Mod+Shift+P after an edit); the
scripts (through `fanshell-qs-conf`) read it each time they run.

```ini
# ~/.config/fanshell-qs/shell.conf
vpn.tunnels=1
wallpaper.dir=~/Pictures/wallpapers
wallpaper.mode=fill
weather.unit=F
ups.host=10.1.0.10
ups.community=public
```

| key | effect |
|---|---|
| `vpn.tunnels` | number of sequential WireGuard tunnels (`tun1`…`tunN`; default `1`, maximum `32`) |
| `wallpaper.dir` | wallpaper directory (default `~/Pictures/wallpapers`) |
| `wallpaper.mode` | how a new pick is laid out on an output, as feh's modes: `fill` (default), `center`, `max`, `scale`, `tile`; what is already set keeps the mode it was set with |
| `wallpaper.fade` | `off` makes a new pick appear at once instead of cross-fading (for weaker machines); default `on` |
| `wallpaper.fade_ms` | length of that cross-fade in milliseconds (default `500`, up to `10000`) |
| `weather.unit` | `C` for Celsius, default Fahrenheit |
| `ups.host` | the UPS's SNMP card; unset, the power pill shows the CPU+GPU estimate instead (see [UPS](#ups)) |
| `ups.community` | SNMPv1 community (default `public`) |
| `power.saver_below` | battery percent at which, on battery, the power profile switches to Power Saver, and back to the one it found once plugged in; `off` to never (default `50`) |
| `battery.path` | the battery `power_supply` dir(s) to read, space-separated; default every `BAT*` that reports a charge, combined |
| `network.bridge` | path to `net-bridge.sh` for the non-NetworkManager backend (default `/usr/local/bin/net-bridge.sh`) |
| `power.window` | RAPL sample window in seconds (default `0.5`) |
| `power.powercap_dir` | powercap root (default `/sys/class/powercap`) |
| `disk.warn` | percent full at which the disk pill shows (default `90`) |
| `network.speed` | `off` leaves the rates out of the network pill; default `on` |
| `gamemode.auto` | `fullscreen` turns game mode on while the focused window is fullscreen; default off |
| `screenshot.dir` | where screenshots are saved (default `~/Pictures/screenshots`) |
| `menu.source_dir` | this checkout, for the menu's entries that open its files (default `~/projects/window-managers/fanshell-qs`) |

The other files in `~/.config/fanshell-qs` (`wallpaper.conf`,
`vibrance.conf`, `outputs.conf`, `dpms.conf`, `lock.conf`, `keyboard.conf`,
`mouse.conf`, `nightlight.conf`) are written by
the shell's popups; edit those settings there.

One environment variable remains, because it is about the window manager
rather than the shell: `FANSHELL_QS_WM`, the backend to run (`oxwm` or
`qtile`) instead of detecting it.

### UPS

With `ups.host` set, the power pill shows the UPS's measured wall draw.
`fanshell-qs-ups` speaks CyberPower's enterprise MIB (RMCARD205 and similar)
and needs `snmpget` from net-snmp. The card answers only managers in its NMS
list, which looks exactly like a timeout otherwise; the source address is
the one `ip route get <host>` picks.

Power draw, the UPS, RAPL permissions (`tools/install-rapl-udev.sh`) and the
`websites.json` menu work exactly as in fanhypr-qs; see its README.

### Compositor

The scrim that closes popups on an outside click is a transparent window, so
it needs picom (or another compositor) to stay invisible. Each shell window
carries a `_NET_WM_WINDOW_TYPE` that picom's `wintypes` can target: `dock`
(panel), `popup_menu` (popups), `utility` (scrim), `notification` (toasts,
OSD), `dialog` (launcher, wallpaper picker). If your picom draws shadows,
exclude at least the scrim:

```
wintypes: { utility = { shadow = false; }; dock = { shadow = false; }; };
```

To blur behind notification toasts (Hyprland's `layer_rule` blur), use
picom 12+ window rules. The shell shapes the toast window to the cards, so
only the cards are blurred:

```
blur: { method = "dual_kawase"; strength = 4; };
rules = ( { match = "name = 'fanshell-qs-notifications'"; blur-background = true; }, );
```
