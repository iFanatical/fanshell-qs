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
# for a pkg-config module or h:HEADER for a C/C++ header. FEDORA, GENTOO and
# ARCH are the packages that provide it (space-separated), or - where the
# distribution doesn't package it. WHAT says what needs it.
_needs=

need() {
    _needs="$_needs$1|$2|$3|$4|$5
"
}

_have() {
    case $1 in
    pc:*) pkg-config --exists "${1#pc:}" 2>/dev/null ;;
    h:*) printf '#include <%s>\n' "${1#h:}" | ${CXX:-g++} -E -x c++ - >/dev/null 2>&1 ;;
    /*) [ -e "$1" ] ;;
    *) command -v "$1" >/dev/null 2>&1 ;;
    esac
}

# Sets _missing to the need() lines whose CHECK fails.
_find_missing() {
    _missing=
    while IFS='|' read -r _check _fedora _gentoo _arch _what; do
        [ -n "$_check" ] || continue
        _have "$_check" || _missing="$_missing$_check|$_fedora|$_gentoo|$_arch|$_what
"
    done <<EOF_NEEDS
$_needs
EOF_NEEDS
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
    while IFS='|' read -r _check _fedora _gentoo _arch _what; do
        [ -n "$_check" ] || continue
        printf '  %-28s %s\n' "$_check" "$_what"
        case $distro in
        fedora) _p=$_fedora ;;
        gentoo) _p=$_gentoo ;;
        arch) _p=$_arch ;;
        *) _p=- ;;
        esac
        if [ "$_p" = - ]; then
            _unpackaged="$_unpackaged $_check"
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

    case $distro in
    fedora) set -- dnf install -y ;;
    gentoo) set -- emerge --noreplace ;;
    arch) set -- pacman -S --needed --noconfirm ;;
    esac
    # shellcheck disable=SC2086
    set -- "$@" $_pkgs
    printf 'to install them:  sudo %s\n' "$*"

    if [ "${dry_run:-0}" = 1 ]; then
        printf '  would: ask to install them, and stop if declined\n'
        _needs=
        return 0
    fi
    [ -t 0 ] || die "missing dependencies, and no terminal to ask on; install them and run again"
    printf 'missing dependencies, do you want to install them? [y/N] '
    read -r _answer || _answer=
    case $_answer in
    [Yy] | [Yy][Ee][Ss]) ;;
    *) die "missing dependencies; install them and run again" ;;
    esac
    sudo "$@" || die "installing the missing dependencies failed"

    _find_missing
    if [ -n "$_missing" ]; then
        printf 'still missing after installing:\n' >&2
        printf '%s' "$_missing" | while IFS='|' read -r _check _rest; do
            printf '  %s\n' "$_check" >&2
        done
        die "missing dependencies"
    fi
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
