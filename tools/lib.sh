# Shared by the install and link scripts of fanshell-qs and the session
# repos (fanoxwm-qs, fanqtile-qs), which source it from the fanshell-qs
# checkout beside them:
#
#     . "$shell_dir/tools/lib.sh"
#     parse_dry_run "$@"
#
# POSIX sh. Nothing here is specific to a window manager.

# Sets dry_run to 1 for -n/--dry-run, 0 for no argument; anything else is a
# usage error.
parse_dry_run() {
    dry_run=0
    case "${1:-}" in
    "") ;;
    -n | --dry-run) dry_run=1 ;;
    *)
        printf 'usage: %s [--dry-run]\n' "$0" >&2
        exit 2
        ;;
    esac
}

step() { printf '\n==> %s\n' "$*"; }

# Runs the command, or only prints it under --dry-run.
run() {
    if [ "${dry_run:-0}" = 1 ]; then
        printf '  would: %s\n' "$*"
    else
        "$@"
    fi
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

# The scripts build and link as the user and call sudo only to install.
require_user() {
    if [ "$(id -u)" -eq 0 ]; then
        die "run this as your user; it calls sudo for the install"
    fi
}

# Dependencies. An install script lists what it needs with need(), then
# calls install_needed, which reports what is missing and offers to install
# it. Anything still missing after that stops the install.
#
#   need CHECK FEDORA GENTOO ARCH WHAT
#
# CHECK is a command on the PATH, an absolute path that must exist, pc:MODULE
# for a pkg-config module, h:HEADER for a C/C++ header, or fn:FUNCTION for a
# shell function that succeeds when it is there (for a check needing a pipe,
# which the | separated lines cannot hold). FEDORA, GENTOO and
# ARCH are the packages that provide it (space-separated), or - where the
# distribution doesn't package it. WHAT says what needs it.
#
# Between need_optional_begin and need_optional_end, a need is optional: one
# still missing after the install (a Gentoo USE flag emerge does not turn
# on, say) is reported and the install goes on without it.
_needs=
_optional=0

need() {
    _needs="$_needs$1|$2|$3|$4|$5|$_optional
"
}

need_optional_begin() { _optional=1; }
need_optional_end() { _optional=0; }

_have() {
    case $1 in
    pc:*) pkg-config --exists "${1#pc:}" 2>/dev/null ;;
    # Qt's include path too, for headers that include Qt's (LayerShellQt).
    h:*) printf '#include <%s>\n' "${1#h:}" |
        ${CXX:-g++} -E -x c++ $(pkg-config --cflags Qt6Gui 2>/dev/null) - >/dev/null 2>&1 ;;
    /*) [ -e "$1" ] ;;
    fn:*) "${1#fn:}" ;;
    *) command -v "$1" >/dev/null 2>&1 ;;
    esac
}

# Queues (need) everything fanshell-qs needs: building the shell, and what
# fanshell-qs-session runs. tools/install-shell checks it, and the session
# repos' install.sh queue it beside their own needs, so one check (and one
# question) comes before anything is built.
need_shell() {
    # Building the shell. On Gentoo, dev-qt/qtbase needs USE "dbus gui network
    # widgets X" (see the Requirements section of README.md).
    need pkg-config pkgconf-pkg-config dev-util/pkgconf pkgconf "building the shell"
    need make make dev-build/make make "building the shell"
    need "${CXX:-g++}" gcc-c++ sys-devel/gcc gcc "building the shell"
    for module in Qt6Widgets Qt6Gui Qt6Core Qt6DBus Qt6Network; do
        need "pc:$module" qt6-qtbase-devel dev-qt/qtbase qt6-base "the shell"
    done
    for module in xcb xcb-randr xcb-dpms xcb-screensaver xcb-xkb xcb-xinput; do
        need "pc:$module" libxcb-devel x11-libs/libxcb libxcb "the panel, Displays, the lock screen"
    done
    need pc:x11 libX11-devel x11-libs/libX11 libx11 "the XEmbed tray"
    need pc:xcomposite libXcomposite-devel x11-libs/libXcomposite libxcomposite "the XEmbed tray"
    need pc:xdamage libXdamage-devel x11-libs/libXdamage libxdamage "the XEmbed tray"
    need pc:xfixes libXfixes-devel x11-libs/libXfixes libxfixes "the XEmbed tray"
    need pc:xkbfile libxkbfile-devel x11-libs/libxkbfile libxkbfile "the Keyboard settings"
    need pc:libpulse pulseaudio-libs-devel media-libs/libpulse libpulse "volume and microphone"
    # PAM has no pkg-config file everywhere; look for its header instead.
    need h:security/pam_appl.h pam-devel sys-libs/pam pam "the lock screen's password check"
    # fanshell-qs-session: the portals it points D-Bus-activated programs at, and
    # the bus tools it runs.
    need /usr/share/dbus-1/services/org.freedesktop.portal.Desktop.service \
        xdg-desktop-portal sys-apps/xdg-desktop-portal xdg-desktop-portal \
        "file pickers and colour scheme for GTK, Electron and Brave"
    need /usr/share/xdg-desktop-portal/portals/gtk.portal \
        xdg-desktop-portal-gtk sys-apps/xdg-desktop-portal-gtk xdg-desktop-portal-gtk \
        "the portal backend config/xdg-desktop-portal/portals.conf picks"
    need dbus-update-activation-environment dbus-tools sys-apps/dbus dbus \
        "passing the session environment to D-Bus-activated programs"
    # Only where nothing provides /run/user/UID/bus: fanshell-qs-session then
    # starts the session's own bus.
    [ -d /run/systemd/system ] ||
        need dbus-run-session dbus-daemon sys-apps/dbus dbus "a session bus of its own"
    need gsettings glib2 dev-libs/glib glib2 "GTK theme, icons, font and colour scheme"
}

# Queues what the shell needs besides under Hyprland (Wayland): building its
# Wayland side, which the build leaves out without these, and the programs
# it runs there. The Hyprland session's install.sh queues it after
# need_shell, and builds with WAYLAND=1 so a missing one fails the build
# instead of leaving Hyprland support out.
need_shell_wayland() {
    need h:LayerShellQt/Window layer-shell-qt-devel kde-plasma/layer-shell-qt layer-shell-qt \
        "the panel and popups as layer-shell surfaces"
    need pc:wayland-client wayland-devel dev-libs/wayland wayland "the lock screen and idle"
    need pc:wayland-cursor wayland-devel dev-libs/wayland wayland "the lock screen's cursor"
    need wayland-scanner wayland-devel dev-util/wayland-scanner wayland "building the shell"
    need pc:wayland-protocols wayland-protocols-devel dev-libs/wayland-protocols \
        wayland-protocols "the lock screen and idle (ext-session-lock, ext-idle-notify)"
    need pc:xkbcommon libxkbcommon-devel x11-libs/libxkbcommon libxkbcommon \
        "the lock screen's keyboard"
    # Part of qtbase since Qt 6.10 (USE=wayland on Gentoo), qtwayland before.
    qt_plugins=$(pkg-config --variable=libdir Qt6Core 2>/dev/null)/qt6/plugins
    need "$qt_plugins/platforms/libqwayland.so" qt6-qtwayland "dev-qt/qtbase[wayland]" \
        qt6-wayland "Qt's Wayland platform plugin"
    need wl-paste wl-clipboard gui-apps/wl-clipboard wl-clipboard "clipboard history"
    need grim grim gui-apps/grim grim "screenshots and the colour picker"
    need jq jq app-misc/jq jq "the Displays popup's saved layouts"
}

# Checks for need_shell_optional().

# mate-polkit's agent, where the distribution puts it.
_mate_polkit_agent() {
    for _f in /usr/libexec/polkit-mate-authentication-agent-1 \
              /usr/lib/mate-polkit/polkit-mate-authentication-agent-1 \
              /usr/libexec/mate-polkit/polkit-mate-authentication-agent-1; do
        [ -x "$_f" ] && return 0
    done
    return 1
}

# Queues what the shell can use but runs without, each feature left out or
# empty while it is missing (README.md, Optional and Session extras). A
# session repo's install.sh queues it after need_shell and
# need_shell_wayland, so a fresh install has every feature. Video
# wallpapers are fanvid's (fanvid-qs, tools/need.sh), which the session
# repos ask about.
need_shell_optional() {
    need_optional_begin
    need nmcli NetworkManager net-misc/networkmanager networkmanager \
        "the network pill and Wi-Fi list"
    need bluetoothctl bluez net-wireless/bluez bluez-utils "the Bluetooth popup"
    need brightnessctl brightnessctl app-misc/brightnessctl brightnessctl \
        "laptop screen and keyboard backlight"
    need ddcutil ddcutil app-misc/ddcutil ddcutil "external monitor brightness (DDC/CI)"
    need powerprofilesctl power-profiles-daemon sys-power/power-profiles-daemon \
        power-profiles-daemon "the power-profile selector"
    need wg-quick wireguard-tools net-vpn/wireguard-tools wireguard-tools "the VPN pill"
    need snmpget net-snmp-utils net-analyzer/net-snmp net-snmp "UPS wattage (ups.host)"
    need curl curl net-misc/curl curl "the weather"
    need pw-play pipewire-utils media-video/pipewire pipewire "the volume-change sound"
    need xdg-open xdg-utils x11-misc/xdg-utils xdg-utils "the menu opening URLs and files"
    need alacritty alacritty x11-terms/alacritty alacritty "the terminal for Terminal=true entries"
    need xsettingsd xsettingsd x11-misc/xsettingsd xsettingsd "GTK/Qt settings for X programs"
    need fn:_mate_polkit_agent mate-polkit mate-extra/mate-polkit mate-polkit \
        "password prompts for privileged actions"
    need_optional_end
}

# Sets _missing to the need() lines whose CHECK fails.
_find_missing() {
    _missing=
    while IFS='|' read -r _check _fedora _gentoo _arch _what _opt; do
        [ -n "$_check" ] || continue
        _have "$_check" || _missing="$_missing$_check|$_fedora|$_gentoo|$_arch|$_what|$_opt
"
    done <<EOF_NEEDS
$_needs
EOF_NEEDS
    # Whether any of them is required.
    _required=0
    case $_missing in *"|0
"*) _required=1 ;; esac
}

# Reports the optional needs still in _missing, and stops on a required one.
_finish_missing() {
    [ -n "$_missing" ] || return 0
    printf '%s' "$_missing" | while IFS='|' read -r _check _f _g _a _what _opt; do
        if [ "$_opt" = 1 ]; then
            printf 'warning: still missing, left out: %s (%s)\n' "$_check" "$_what" >&2
        else
            printf 'still missing: %s (%s)\n' "$_check" "$_what" >&2
        fi
    done
    # A USE flag the needs' packages want there, set by whoever queued them
    # (fanvid-qs's need_fanvid).
    if [ "${distro:-}" = gentoo ] && [ -n "${gentoo_note:-}" ]; then
        printf '%s\n' "$gentoo_note" >&2
    fi
    [ "$_required" = 0 ] || die "missing dependencies"
}

# Sets distro to fedora, gentoo or arch, from /etc/os-release (ID, then
# ID_LIKE, so derivatives count), or to unknown.
detect_distro() {
    distro=unknown
    [ -r /etc/os-release ] || return 0
    for _id in $(. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}"); do
        case $_id in
        fedora | gentoo | arch)
            distro=$_id
            return 0
            ;;
        esac
    done
}

install_needed() {
    _find_missing
    if [ -z "$_missing" ]; then
        _needs=
        echo ok
        return 0
    fi

    detect_distro
    _pkgs=
    _unpackaged=
    printf 'missing dependencies:\n'
    while IFS='|' read -r _check _fedora _gentoo _arch _what _opt; do
        [ -n "$_check" ] || continue
        if [ "$_opt" = 1 ]; then
            printf '  %-28s %s (optional)\n' "$_check" "$_what"
        else
            printf '  %-28s %s\n' "$_check" "$_what"
        fi
        case $distro in
        fedora) _p=$_fedora ;;
        gentoo) _p=$_gentoo ;;
        arch) _p=$_arch ;;
        *) _p=- ;;
        esac
        if [ "$_p" = - ]; then
            [ "$_opt" = 1 ] || _unpackaged="$_unpackaged $_check"
            continue
        fi
        for _one in $_p; do
            case " $_pkgs " in
            *" $_one "*) ;;
            *) _pkgs="$_pkgs $_one" ;;
            esac
        done
    done <<EOF_MISSING
$_missing
EOF_MISSING

    [ "$distro" != unknown ] ||
        die "unknown distribution (not Fedora, Gentoo or Arch); install these yourself and run again"
    [ -z "$_unpackaged" ] ||
        die "not packaged by $distro, install these yourself and run again:$_unpackaged"

    # Fedora: ffmpeg and the GPUs' H.264/HEVC drivers are RPM Fusion's.
    _rpmfusion=
    if [ "$distro" = fedora ]; then
        for _one in $_pkgs; do
            case $_one in
            ffmpeg | ffmpeg-devel | mesa-va-drivers-freeworld) _rpmfusion="$_rpmfusion free" ;;
            intel-media-driver) _rpmfusion="$_rpmfusion free nonfree" ;;
            esac
        done
    fi

    case $distro in
    # --allowerasing: RPM Fusion's ffmpeg replaces ffmpeg-free, and
    # mesa-va-drivers-freeworld mesa-va-drivers.
    fedora) set -- dnf install -y --allowerasing ;;
    gentoo) set -- emerge --noreplace ;;
    arch) set -- pacman -S --needed --noconfirm ;;
    esac
    # shellcheck disable=SC2086
    set -- "$@" $_pkgs
    printf 'to install them:  sudo %s\n' "$*"

    if [ "${dry_run:-0}" = 1 ]; then
        printf '  would: ask to install them, and stop if declined while a required one is missing\n'
        _needs=
        return 0
    fi
    if [ ! -t 0 ]; then
        [ "$_required" = 0 ] ||
            die "missing dependencies, and no terminal to ask on; install them and run again"
        _finish_missing
        _needs=
        return 0
    fi
    printf 'missing dependencies, do you want to install them? [y/N] '
    read -r _answer || _answer=
    case $_answer in
    [Yy] | [Yy][Ee][Ss]) ;;
    *)
        [ "$_required" = 0 ] || die "missing dependencies; install them and run again"
        _finish_missing
        _needs=
        return 0
        ;;
    esac
    for _repo in $(printf '%s\n' $_rpmfusion | sort -u); do
        _have /etc/yum.repos.d/rpmfusion-$_repo.repo && continue
        printf 'enabling RPM Fusion (%s)\n' "$_repo"
        sudo dnf install -y \
            "https://mirrors.rpmfusion.org/$_repo/fedora/rpmfusion-$_repo-release-$(rpm -E %fedora).noarch.rpm" ||
            die "enabling RPM Fusion ($_repo) failed"
    done
    sudo "$@" || die "installing the missing dependencies failed"

    _find_missing
    _finish_missing
    _needs=
    echo ok
}

# link SOURCE TARGET: make TARGET (in $HOME) a symlink to SOURCE (in a
# checkout). A link already pointing at SOURCE is left alone; a link
# pointing elsewhere is repointed (this is how a session repo takes
# ~/.xinitrc over from the other); a real file in the way is moved aside to
# TARGET.bak-DATE, never overwritten.
link() {
    _source=$1
    _target=$2

    if [ ! -e "$_source" ]; then
        printf 'missing %s, skipped\n' "$_source" >&2
        return
    fi
    if [ -L "$_target" ] && [ "$(readlink "$_target")" = "$_source" ]; then
        printf 'ok      %s\n' "$_target"
        return
    fi

    run mkdir -p "$(dirname -- "$_target")"
    if [ -L "$_target" ]; then
        printf 'relink  %s (was -> %s)\n' "$_target" "$(readlink "$_target")"
        run rm -f "$_target"
    elif [ -e "$_target" ]; then
        _stamp=$(date +%Y%m%d-%H%M%S)
        printf 'backup  %s -> %s.bak-%s\n' "$_target" "$_target" "$_stamp"
        run mv "$_target" "$_target.bak-$_stamp"
    else
        printf 'link    %s\n' "$_target"
    fi
    run ln -s "$_source" "$_target"
}
