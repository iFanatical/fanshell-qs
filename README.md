# fanshell-qs

The hand-rolled C++/Qt bar and desktop shell for X11, shared by two
sessions:

- [fanoxwm-qs](https://github.com/iFanatical/fanoxwm-qs): oxwm, with the
  IPC patch the shell talks to, and its Lua config.
- [fanqtile-qs](https://github.com/iFanatical/fanqtile-qs): qtile and its
  config.

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
│   ├── tools/                link-config, i3lock-color and RAPL installers
│   └── config/               sxhkdrc, picom.conf, websites.json, output defaults
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
    libXdamage-devel libXfixes-devel pulseaudio-libs-devel \
    zig libXft-devel libXinerama-devel fontconfig-devel \
    xorg-x11-server-Xorg xorg-x11-xinit picom xrandr sxhkd \
    jq xdotool xprop feh NetworkManager bluez upower brightnessctl \
    ddcutil power-profiles-daemon wireguard-tools net-snmp-utils curl \
    pipewire-utils alacritty xdg-utils xset xinput xsetroot xsettingsd \
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
is moved to `NAME.bak-DATE`, never overwritten.

Optional, from this repo's `tools/`: `install-i3lock-color` for the themed
lock screen (its header lists the packages it needs to build; without it
the lock screen is a plain colour), and `sudo ./install-rapl-udev.sh` for
CPU wattage in the power pill.

**5. Log in.** `startx` from a text console starts oxwm, which starts the
bar.

**Not in these repos**, but the session expects them:

- wallpapers in `~/Pictures/wallpapers` (or `$FANSHELL_QS_WALLPAPER_DIR`);
- the Tokyo-Night GTK theme and Tela-circle icons that `xinitrc` sets
  through `gsettings`;
- the helper scripts `config/sxhkdrc` calls from `$SCRIPTS`
  (`~/.local/bin`): `screenshot.sh` (Print, Ctrl+Alt+Tab) needs maim,
  xclip, xdotool and notify-send (Fedora `maim xclip xdotool libnotify`;
  Gentoo `media-gfx/maim x11-misc/xclip x11-misc/xdotool x11-libs/libnotify`),
  `xcolorpick.sh` (Alt+C) needs xcolor, which neither distribution packages
  (`cargo install xcolor`), plus xclip and notify-send, and
  `alacritty-opacity` (Mod+Shift+-/=);
- the rest of your dotfiles: `~/.zshrc`, alacritty, nvim;
- `~/.config/fanshell-qs`, the shell's saved settings (wallpaper,
  vibrance, display layouts, screen power and lock). It starts empty with
  defaults; copy it from another machine to keep the same choices.

**Switching to qtile** later: `sudo dnf install qtile` (Gentoo:
`sudo emerge x11-wm/qtile`), then `./install.sh` in fanqtile-qs, which
points `~/.xinitrc` at qtile and reinstalls the shell; `./install.sh` in
fanoxwm-qs switches back. fanqtile-qs's README lists what else the qtile
side uses.

**On Gentoo**, the steps are the same with the `emerge` lines under
[Installing](#installing) and in fanoxwm-qs's README, and
`x11-misc/i3lock-color` from portage instead of `install-i3lock-color`.

## Window managers

The shell talks to the window manager only through `WmState`
(`shell/wmstate.h`): the outputs, each one's workspaces, layout and focused
title, and a few actions. A backend fills it in:

| backend | window manager | status |
|---|---|---|
| `oxwmstate.*` | the patched oxwm from fanoxwm-qs, over its IPC sockets | complete |
| `qtilestate.*`, `qtileipc.*` | qtile, over its own IPC socket; hooks in its config send `wm changed` (fanqtile-qs does this) | complete |

At startup the shell picks the backend for the window manager that is
running: `$FANSHELL_QS_WM` if set, else the name the window manager gives
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
| libxcb (with RandR, DPMS and the screen saver extension) | `libxcb-devel` | `x11-libs/libxcb` | the panel's dock surface, the Displays popup, digital vibrance, Screen power, lock on idle |
| X11, XComposite, XDamage, XFixes | `libX11-devel` `libXcomposite-devel` `libXdamage-devel` `libXfixes-devel` | `x11-libs/libX11` `x11-libs/libXcomposite` `x11-libs/libXdamage` `x11-libs/libXfixes` | the XEmbed tray |
| libpulse | `pulseaudio-libs-devel` | `media-libs/libpulse` | volume and microphone control; works against pipewire-pulse |
| An X server and xinit | `xorg-x11-server-Xorg` `xorg-x11-xinit` | `x11-base/xorg-server` `x11-apps/xinit` | running the session from `~/.xinitrc` with `startx` (XLibre works too) |
| A D-Bus session bus | `dbus` | `sys-apps/dbus` | notifications, the StatusNotifierItem tray, media controls |
| picom, or another compositor | `picom` | `x11-misc/picom` | translucent panel and toasts, and the popup scrim, which is otherwise drawn opaque |
| xrandr | `xrandr` | `x11-apps/xrandr` | applying output layouts, at login and from the Displays popup |
| sxhkd | `sxhkd` | `x11-misc/sxhkd` | every program, popup and media hotkey in `config/sxhkdrc`; the window manager only binds window management |
| A Nerd Font | — | — | `JetBrainsMono Nerd Font Propo` for the bar's icons, from [nerdfonts.com](https://www.nerdfonts.com) |

### Optional

Each one enables a single feature. Anything missing just leaves that feature
out or empty.

| What | Fedora | Gentoo | For |
|---|---|---|---|
| feh | `feh` | `media-gfx/feh` | setting the wallpaper from the picker, and restoring it at startup |
| NetworkManager | `NetworkManager` | `net-misc/networkmanager` | the network pill and Wi-Fi list (`nmcli`) |
| BlueZ | `bluez` | `net-wireless/bluez` | the Bluetooth popup (`bluetoothctl`) |
| UPower | `upower` | `sys-power/upower` | the battery pill |
| brightnessctl | `brightnessctl` | `app-misc/brightnessctl` | laptop screen and keyboard backlight |
| ddcutil | `ddcutil` | `app-misc/ddcutil` | external monitor brightness over DDC/CI (needs the `i2c-dev` module and access to `/dev/i2c-*`) |
| power-profiles-daemon | `power-profiles-daemon` | `sys-power/power-profiles-daemon` | the System dropdown's power-profile selector |
| wireguard-tools | `wireguard-tools` | `net-vpn/wireguard-tools` | the VPN pill; `wg-quick` runs through passwordless `sudo -n` |
| net-snmp | `net-snmp-utils` | `net-analyzer/net-snmp` | UPS wattage in the power pill, when `FANSHELL_QS_UPS_HOST` is set |
| curl | `curl` | `net-misc/curl` | the weather pill (wttr.in) |
| pw-play or paplay | `pipewire-utils` or `pulseaudio-utils` | `media-video/pipewire` | the optional volume-change sound |
| i3lock-color | `tools/install-i3lock-color` (not packaged) | `x11-misc/i3lock-color` | the themed, blurred lock screen (`fanshell-qs-lock`), for the Lock button, lock on idle and lock before sleep. Stock `i3lock` gives a plain Tokyo Night background instead; without either, xsecurelock, slock, xscreensaver and then `loginctl lock-session` are tried |
| A terminal | `alacritty` or `xterm` | `x11-terms/alacritty` or `x11-terms/xterm` | `Terminal=true` launcher entries and the menu's actions (`$TERMINAL`, then alacritty for the menu, xterm for the launcher) |
| xdg-utils | `xdg-utils` | `x11-misc/xdg-utils` | the menu opening URLs and files |

### Session extras

The session repos' `xinitrc` also starts or calls these. None of them are
needed by the shell, so drop the line from `~/.xinitrc` instead of installing
something you don't want.

| What | Fedora | Gentoo | For |
|---|---|---|---|
| xset | `xset` | `x11-apps/xset` | key repeat rate (`xset r rate 200 35`) |
| xinput | `xinput` | `x11-apps/xinput` | per-mouse pointer acceleration |
| xsetroot | `xsetroot` | `x11-apps/xsetroot` | the root window's arrow cursor |
| xsettingsd | `xsettingsd` | `x11-misc/xsettingsd` | GTK/Qt theme and font settings for X apps |
| glib2 (`gsettings`) | `glib2` | `dev-libs/glib` | GTK theme, icons, font and colour scheme |
| A polkit agent | `mate-polkit` | `mate-extra/mate-polkit` | password prompts for privileged actions |
| gpu-screen-recorder-ui | — | — | `gsr-ui launch-daemon`, screen recording |

### Installing

Required:

```sh
# Fedora
sudo dnf install gcc gcc-c++ make pkgconf-pkg-config \
    qt6-qtbase-devel libxcb-devel libX11-devel libXcomposite-devel \
    libXdamage-devel libXfixes-devel pulseaudio-libs-devel \
    xorg-x11-server-Xorg xorg-x11-xinit picom xrandr sxhkd

# Gentoo
sudo emerge dev-qt/qtbase x11-libs/libxcb x11-libs/libX11 \
    x11-libs/libXcomposite \
    x11-libs/libXdamage x11-libs/libXfixes media-libs/libpulse \
    x11-base/xorg-server x11-apps/xinit x11-misc/picom x11-apps/xrandr \
    x11-misc/sxhkd
```

Optional, everything above:

```sh
# Fedora
sudo dnf install feh NetworkManager bluez upower \
    brightnessctl ddcutil power-profiles-daemon wireguard-tools \
    net-snmp-utils curl pipewire-utils alacritty xdg-utils \
    xset xinput xsetroot xsettingsd mate-polkit
tools/install-i3lock-color   # builds it; needs the deps listed in its header

# Gentoo
sudo emerge media-gfx/feh \
    net-misc/networkmanager net-wireless/bluez sys-power/upower \
    app-misc/brightnessctl app-misc/ddcutil sys-power/power-profiles-daemon \
    net-vpn/wireguard-tools net-analyzer/net-snmp net-misc/curl \
    media-video/pipewire x11-terms/alacritty \
    x11-misc/xdg-utils x11-apps/xset x11-apps/xinput x11-apps/xsetroot \
    x11-misc/xsettingsd mate-extra/mate-polkit x11-misc/i3lock-color
```

## Build

```sh
make                       # shell/fanshell-qs-shell
sudo make install          # shell and helper scripts into /usr/local/bin
make link-config           # link this repo's config files into $HOME
```

Or `tools/install-shell`, which also checks the build dependencies, moves a
running session onto the new build (restarting the bar and reloading sxhkd)
and removes the files fanoxwm-qs installed before the split. The session
repos' `install.sh` run it as their last step.

Then set up a session from fanoxwm-qs or fanqtile-qs: each starts the shell
from its window manager's config and links `~/.xinitrc`, `~/.X_envars` and
the window manager's own config.

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
| `config/websites.json` | `~/.config/fanshell-qs/websites.json` (the menu's Websites page) |
| `config/outputs.defaults.conf` | `~/.config/fanshell-qs/outputs.defaults.conf` (fallback output layouts) |

The tray and toasts live on X's primary output (`xrandr --primary`). The
launcher, wallpaper picker and Control Panel open on the output the window
manager has focused.

### What the shell remembers

Everything the shell persists lives in `~/.config/fanshell-qs/` (or
`$XDG_CONFIG_HOME/fanshell-qs/`), never under `~/.cache`, so clearing the
cache loses nothing you chose:

| file | |
|---|---|
| `wallpaper.conf` | the picker's last wallpaper and feh mode; re-applied when the shell starts |
| `vibrance.conf` | digital vibrance per output; re-applied at start and when an output connects |
| `outputs.conf` | output layouts, one per set of connected outputs; applied from `.xinitrc` and on hotplug |
| `dpms.conf` | screen blanking and DPMS timeouts; applied at startup |
| `lock.conf` | lock on idle, before sleep, and the fullscreen pause |
| `websites.json` | linked from the repo (above) |
| `sounds/` | optional `audio-volume-change.*` for volume feedback |

The shell runs feh with `--no-fehbg`, so it no longer writes `~/.fehbg`. If
`wallpaper.conf` does not exist yet, the path in an old `~/.fehbg` is imported
once. After that, `~/.fehbg` can be deleted. Runtime-only state (IPC sockets,
scratchpad window ids, the `maximize` layout memo) stays in
`$XDG_RUNTIME_DIR`: it means nothing after a reboot.

### Control Panel

The System dropdown is for status and quick actions: CPU, memory and
temperature, weather, Bluetooth, clipboard, VPN, the power profile, audio,
and one row of icons for Lock, Sleep, Log out, Reboot and Power off (named
in the caption below the row as you point at them; the last three ask for a
second click). Settings live in the **Control Panel**, opened from the
dropdown's header (or `ipc call controlpanel toggle`). It is a grid of
tiles, each showing a line of live status and opening its own window:
Displays, Screen power, Screen lock and Wallpaper. A new settings window
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
`.xinitrc` runs `fanshell-qs-outputs apply` at login. When the connected set
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

### Screen lock

The Lock button, and every automatic lock, run `fanshell-qs-lock`: i3lock-color
over a blurred copy of the screen, with the ring, clock and messages in Tokyo
Night colours (the palette is at the top of the script). There is no password
box; the ring lights up as you type, turns cyan while checking and red on a
wrong password.

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

`ipc call lock now` locks straight away. Settings are saved to `lock.conf`;
without one, the screen locks after 10 minutes, before sleep, and pauses for
fullscreen.

## Bringing over fanhypr-qs changes

The shell is a port, not a fork with shared history. Most files differ from
fanhypr-qs only by the `fanhypr` → `fanshell` rename, so a change there usually
applies directly:

```sh
git -C ../fanhypr-qs diff 9ea1f08..HEAD -- shell scripts \
  | sed 's/fanhypr/fanshell/g; s/FANHYPR/FANSHELL/g' \
  | git apply --reject
```

These files are X11 rewrites, and changes to their fanhypr-qs
counterparts need porting by hand:

| fanhypr-qs | fanshell-qs |
|---|---|
| `hypripc.*` | `oxwmipc.*` |
| `hyprstate.*` | `wmstate.*` (the window-manager-neutral model every widget uses), with `oxwmstate.*` and `qtilestate.*` as its backends |
| `wlutil.*` | `surface.*` |
| `clipboard.cpp` (`wl-paste`) | `clipboard.cpp` (`QClipboard`) |
| `wallpaper.cpp` (`awww`) | `wallpaper.cpp` (`feh`) |
| `panel.cpp` workspaces, `pills.*` submap | tags, layout pill |

Update the commit in this README when you do.

## Configuration

Everything visual lives in `shell/theme.h`. Runtime overrides come from the
shell's environment, which it inherits from the window manager, so set them
in `.xinitrc` before it starts the window manager.

| variable | effect |
|---|---|
| `FANSHELL_QS_WM` | the backend to run, `oxwm` or `qtile`, instead of detecting it |
| `FANSHELL_TUN_COUNT` | number of sequential WireGuard tunnels (`tun1`…`tunN`; default `1`, maximum `32`) |
| `FANSHELL_QS_BATTERY_PATH` | battery `power_supply` dir (multi-battery machines) |
| `FANSHELL_QS_WEATHER_UNIT` | `C` for Celsius, default Fahrenheit |
| `FANSHELL_QS_NET_BRIDGE` | path to `net-bridge.sh` for the non-NetworkManager backend |
| `FANSHELL_QS_WALLPAPER_DIR` | wallpaper directory (default `~/Pictures/wallpapers`) |
| `FANSHELL_QS_WALLPAPER_MODE` | feh mode: `fill` (default), `center`, `max`, `scale`, `tile` |
| `FANSHELL_QS_POWER_WINDOW` | RAPL sample window in seconds (default `0.5`) |
| `FANSHELL_QS_POWERCAP_DIR` | powercap root (default `/sys/class/powercap`) |
| `FANSHELL_QS_UPS_HOST` | SNMP card address; unset disables UPS polling |
| `FANSHELL_QS_UPS_COMMUNITY` | SNMPv1 community (default `public`) |

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
