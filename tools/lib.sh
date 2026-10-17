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
