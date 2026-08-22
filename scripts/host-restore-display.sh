#!/usr/bin/env bash
# Recover a KDE display layout from a TTY or SSH session without assuming a
# particular streaming application or connector name.
set -euo pipefail

usage()
{
	cat >&2 <<EOF
Usage: $0 --physical OUTPUT [--virtual OUTPUT] [--stop PROCESS ...]
          [--grace-seconds N]

Re-enables OUTPUT as the primary display and optionally disables a virtual
output. Each named process receives SIGTERM first; SIGKILL is used only if it
is still running after the grace period.

Example:
  $0 --physical HDMI-A-1 --virtual Virtual-1 --stop my-stream-server
EOF
}

PHYSICAL_OUTPUT=
VIRTUAL_OUTPUT=
GRACE_SECONDS=5
STOP_PROCESSES=()

while [ "$#" -gt 0 ]; do
	case "$1" in
	--physical)
		[ "$#" -ge 2 ] || { usage; exit 2; }
		PHYSICAL_OUTPUT=$2
		shift 2
		;;
	--virtual)
		[ "$#" -ge 2 ] || { usage; exit 2; }
		VIRTUAL_OUTPUT=$2
		shift 2
		;;
	--stop)
		[ "$#" -ge 2 ] || { usage; exit 2; }
		STOP_PROCESSES+=("$2")
		shift 2
		;;
	--grace-seconds)
		[ "$#" -ge 2 ] || { usage; exit 2; }
		GRACE_SECONDS=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		usage
		exit 2
		;;
	esac
done

[ -n "$PHYSICAL_OUTPUT" ] || { usage; exit 2; }
case "$GRACE_SECONDS" in
	''|*[!0-9]*)
		printf 'invalid --grace-seconds value: %s\n' "$GRACE_SECONDS" >&2
		exit 2
		;;
esac
[ "$GRACE_SECONDS" -le 300 ] || {
	printf -- '--grace-seconds must not exceed 300\n' >&2
	exit 2
}
GRACE_TICKS=$((10#$GRACE_SECONDS * 10))
command -v kscreen-doctor >/dev/null || {
	printf 'kscreen-doctor is required\n' >&2
	exit 1
}

for process in "${STOP_PROCESSES[@]}"; do
	[ -n "$process" ] || { printf 'empty --stop process name\n' >&2; exit 2; }
	if ! pgrep -x -- "$process" >/dev/null 2>&1; then
		printf '### process not running: %s\n' "$process"
		continue
	fi
	printf '### stopping %s with SIGTERM\n' "$process"
	pkill -TERM -x -- "$process" 2>/dev/null || true
	for ((elapsed = 0; elapsed < GRACE_TICKS; elapsed++)); do
		pgrep -x -- "$process" >/dev/null 2>&1 || break
		sleep 0.1
	done
	if pgrep -x -- "$process" >/dev/null 2>&1; then
		printf '### %s did not stop; sending SIGKILL\n' "$process" >&2
		pkill -KILL -x -- "$process" 2>/dev/null || true
	fi
done

printf '### enabling physical output %s as primary\n' "$PHYSICAL_OUTPUT"
kscreen-doctor "output.${PHYSICAL_OUTPUT}.enable" \
	"output.${PHYSICAL_OUTPUT}.priority.1"
if [ -n "$VIRTUAL_OUTPUT" ]; then
	printf '### disabling virtual output %s\n' "$VIRTUAL_OUTPUT"
	kscreen-doctor "output.${VIRTUAL_OUTPUT}.disable"
fi

printf '### current layout:\n'
kscreen-doctor -o | sed -n '1,24p'
printf '%s\n' \
	'### recovery commands completed.' \
	'### If the screen is still dark, restart the compositor from the session.'
