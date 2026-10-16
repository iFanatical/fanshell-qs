#!/bin/sh
# Grant a group read access to the kernel's RAPL energy counters, so the bar's
# power-draw widget can report CPU package watts without running as root.
#
#     sudo tools/install-rapl-udev.sh [group]
#
# WHY THIS IS NEEDED
#
# /sys/class/powercap/intel-rapl:*/energy_uj is mode 0400 on most distributions.
# That is deliberate: RAPL samples finely enough that a local unprivileged
# process watching it can infer secrets from another process's power signature
# (PLATYPUS, CVE-2020-8694), so the counter was restricted to root.
#
# Relaxing it to a group re-opens that side channel to members of that group.
# On a single-user desktop that is usually an easy trade; on a shared or
# multi-tenant machine it is not. Decide deliberately -- this script will not
# make the file world-readable.
#
# Nothing here is needed for GPU wattage: amdgpu's hwmon power sensors are
# already world-readable. This is only about the CPU half.
#
# Undo with:  rm /etc/udev/rules.d/99-fanshell-qs-rapl.rules && reboot
set -eu

GROUP="${1:-power}"
RULE=/etc/udev/rules.d/99-fanshell-qs-rapl.rules

if [ "$(id -u)" -ne 0 ]; then
	echo "error: must run as root (sudo $0)" >&2
	exit 1
fi

if [ ! -d /sys/class/powercap ]; then
	echo "error: no /sys/class/powercap -- this kernel exposes no RAPL zones," >&2
	echo "       so there is nothing to grant access to." >&2
	exit 1
fi

if ! getent group "$GROUP" >/dev/null 2>&1; then
	echo "creating group '$GROUP'"
	groupadd --system "$GROUP"
fi

CHGRP=$(command -v chgrp)
CHMOD=$(command -v chmod)

cat > "$RULE" <<EOF
# Installed by fanshell-qs tools/install-rapl-udev.sh
#
# Let members of "$GROUP" read the RAPL energy counters, which are root-only by
# default as a PLATYPUS (CVE-2020-8694) mitigation. Re-applied on every add so
# it survives reboots and module reloads.
SUBSYSTEM=="powercap", ACTION=="add|change", \\
    RUN+="$CHGRP $GROUP /sys%p/energy_uj", \\
    RUN+="$CHMOD g+r /sys%p/energy_uj"
EOF
echo "wrote $RULE"

udevadm control --reload-rules
udevadm trigger --subsystem-match=powercap --action=add
# The trigger is asynchronous; give the RUN= helpers a moment to land.
udevadm settle 2>/dev/null || sleep 1

# Apply directly as well: a zone whose device event already fired before the
# rule existed would otherwise stay root-only until the next boot.
for z in /sys/class/powercap/intel-rapl:*; do
	[ -e "$z/energy_uj" ] || continue
	"$CHGRP" "$GROUP" "$z/energy_uj" 2>/dev/null || true
	"$CHMOD" g+r "$z/energy_uj" 2>/dev/null || true
done

echo
echo "result:"
ok=0
for z in /sys/class/powercap/intel-rapl:*; do
	[ -e "$z/energy_uj" ] || continue
	printf '  %-34s %s  %s\n' "$z/energy_uj" \
		"$(stat -c '%U:%G %a' "$z/energy_uj")" \
		"$(cat "$z/name" 2>/dev/null)"
	ok=1
done
[ "$ok" = 1 ] || { echo "  (no energy_uj found)"; exit 1; }

echo
# SUDO_USER is the human who invoked us, not root.
WHO="${SUDO_USER:-$(id -un)}"
if id -nG "$WHO" 2>/dev/null | tr ' ' '\n' | grep -qx "$GROUP"; then
	echo "'$WHO' is already in '$GROUP' -- restart the bar and the CPU"
	echo "reading will appear."
else
	echo "'$WHO' is NOT in '$GROUP'. Add them and log out and back in"
	echo "(group membership is fixed at login):"
	echo "    sudo usermod -aG $GROUP $WHO"
fi
