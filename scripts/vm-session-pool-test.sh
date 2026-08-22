#!/usr/bin/env bash
# Hermes-KMS host-compatible automatic private-session pool smoke test.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
RUNTIME_RULE=/run/udev/rules.d/70-hermes-kms-session-seats.rules
RULE_BACKUP=""
MODULE_LOADED_BY_TEST=0
CARDS=()
FAIL=0

cleanup()
{
	if [ "$MODULE_LOADED_BY_TEST" -eq 1 ]; then
		timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
		MODULE_LOADED_BY_TEST=0
	fi
	if [ -n "$RULE_BACKUP" ]; then
		cp -- "$RULE_BACKUP" "$RUNTIME_RULE"
		rm -f -- "$RULE_BACKUP"
		RULE_BACKUP=""
	else
		rm -f -- "$RUNTIME_RULE"
	fi
	udevadm control --reload-rules 2>/dev/null || true
}
trap cleanup EXIT

value()
{
	local key="$1"
	awk -F= -v key="$key" '$1 == key { print $2; exit }'
}

require_value()
{
	local text="$1"
	local key="$2"
	local expected="$3"
	local actual

	actual="$(printf '%s\n' "$text" | value "$key")"
	if [ "$actual" != "$expected" ]; then
		printf 'FAIL: %s expected %s, got %s\n' \
			"$key" "$expected" "${actual:-<empty>}" >&2
		FAIL=1
	fi
}

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root (virtme-ng --exec runs as root by default)\n' >&2
	exit 1
}
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
[ -x "$CTL" ] || { printf 'control tool not built: %s\n' "$CTL" >&2; exit 1; }
if grep -q '^hermes_kms ' /proc/modules 2>/dev/null; then
	printf 'refusing to replace an already-loaded hermes_kms instance\n' >&2
	exit 1
fi

if [ -e "$RUNTIME_RULE" ]; then
	RULE_BACKUP="$(mktemp /tmp/hermes-udev-rule.XXXXXX)"
	cp -- "$RUNTIME_RULE" "$RULE_BACKUP"
fi
install -Dm0644 "$REPO/udev/70-hermes-kms-session-seats.rules" "$RUNTIME_RULE"
udevadm control --reload-rules
DMESG_MARK="$(dmesg | wc -l)"
insmod "$KO" initial_enabled=0 hotplug_events=0 session_devices=2 outputs=1
MODULE_LOADED_BY_TEST=1
udevadm trigger --subsystem-match=drm --action=change
udevadm settle
sleep 0.5

[ "$(cat /sys/module/hermes_kms/parameters/session_devices)" = 2 ] || {
	printf 'FAIL: session_devices parameter is not 2\n' >&2
	exit 1
}

for card in /dev/dri/card*; do
	[ -e "$card" ] || continue
	if "$CTL" --device "$card" version >/dev/null 2>&1; then
		CARDS+=("$card")
	fi
done
[ "${#CARDS[@]}" -eq 3 ] || {
	printf 'FAIL: expected host + two session cards, found %u\n' \
		"${#CARDS[@]}" >&2
	exit 1
}

mapfile -t CARDS < <(
	for card in "${CARDS[@]}"; do
		index="$("$CTL" --device "$card" identity | value device_index)"
		printf '%s %s\n' "$index" "$card"
	done | sort -n | awk '{ print $2 }'
)

for index in 0 1 2; do
	identity="$("$CTL" --device "${CARDS[$index]}" identity)"
	caps="$("$CTL" --device "${CARDS[$index]}" caps)"
	require_value "$identity" device_index "$((index + 1))"
	require_value "$identity" device_count 3
	require_value "$identity" session_device_count 2
	require_value "$caps" multi_device true
	require_value "$caps" session_device_pool true
	if [ "$index" -eq 0 ]; then
		require_value "$identity" device_role host
		require_value "$identity" session_index 0
	else
		require_value "$identity" device_role session
		require_value "$identity" session_index "$index"
	fi
done

host_properties="$(udevadm info --query=property --name="${CARDS[0]}")"
if printf '%s\n' "$host_properties" | grep -q '^ID_SEAT=hermes-kms-'; then
	printf 'FAIL: host card was moved away from seat0\n' >&2
	FAIL=1
fi
for index in 1 2; do
	# The change event can race the udev database write under a loaded
	# host: a query served from the db written before the event finished
	# shows no ID_SEAT even though the rule matches. Poll briefly instead
	# of reading once after a fixed sleep.
	properties=
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		properties="$(udevadm info --query=property --name="${CARDS[$index]}")"
		printf '%s\n' "$properties" | grep -q '^ID_SEAT=' && break
		# The ruleset reload is asynchronous: a trigger fired right after
		# --reload-rules can be processed against the previous rules, and
		# udevadm info only reads the database. Re-fire the event and wait
		# until the daemon stores the seat assignment.
		udevadm trigger --subsystem-match=drm --action=change
		udevadm settle
		sleep 0.5
	done
	require_value "$properties" ID_SEAT "hermes-kms-$index"
	if [ "$(stat -c '%u:%g:%a' "${CARDS[$index]}")" != '0:0:600' ]; then
		printf 'FAIL: private card %s is not root:root mode 0600\n' \
			"${CARDS[$index]}" >&2
		FAIL=1
	fi
	if printf '%s\n' "$properties" | grep -Eq '^CURRENT_TAGS=.*uaccess'; then
		printf 'FAIL: private card %s retained the uaccess tag\n' \
			"${CARDS[$index]}" >&2
		FAIL=1
	fi
	wants="$(printf '%s\n' "$properties" |
		awk -F= '$1 == "SYSTEMD_WANTS" { print $2; exit }')"
	case " $wants " in
		*" hermes-kms-seatd@${index}.service "*) ;;
		*)
			printf 'FAIL: card %s did not request broker instance %s\n' \
				"${CARDS[$index]}" "$index" >&2
			FAIL=1
			;;
	esac
done

require_value "hermes_kms_role=$(cat /sys/devices/platform/hermes-kms.0/hermes_kms_role)" \
	hermes_kms_role host
require_value "hermes_kms_role=$(cat /sys/devices/platform/hermes-kms.1/hermes_kms_role)" \
	hermes_kms_role session
require_value "hermes_kms_session_index=$(cat /sys/devices/platform/hermes-kms.1/hermes_kms_session_index)" \
	hermes_kms_session_index 1
require_value "hermes_kms_session_index=$(cat /sys/devices/platform/hermes-kms.2/hermes_kms_session_index)" \
	hermes_kms_session_index 2

if ! timeout -k 1s 5s rmmod hermes_kms; then
	printf 'FAIL: module did not unload cleanly\n' >&2
	FAIL=1
fi
SPLAT="$(dmesg | tail -n +"$((DMESG_MARK + 1))" | grep -iE \
	'BUG:|KASAN|use-after-free|WARNING:|RIP:|slab corruption|Redzone|Poison|refcount|general protection|null pointer' \
	|| true)"
if [ -n "$SPLAT" ]; then
	printf '%s\n' "$SPLAT" >&2
	printf 'FAIL: kernel splat detected\n' >&2
	FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
	dmesg | tail -n 160 >&2 || true
	exit "$FAIL"
fi

printf 'PASS: one host card and two private session cards were identified without manual device counting\n'
