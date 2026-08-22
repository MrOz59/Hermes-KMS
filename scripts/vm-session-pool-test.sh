#!/usr/bin/env bash
# Hermes-KMS host-compatible automatic private-session pool smoke test.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
RUNTIME_RULE=/run/udev/rules.d/70-hermes-kms-session-seats.rules
CARDS=()
FAIL=0

cleanup()
{
	timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	rm -f "$RUNTIME_RULE"
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

install -Dm0644 "$REPO/udev/70-hermes-kms-session-seats.rules" "$RUNTIME_RULE"
udevadm control --reload-rules
insmod "$KO" initial_enabled=0 hotplug_events=0 session_devices=2 outputs=1
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
	properties="$(udevadm info --query=property --name="${CARDS[$index]}")"
	require_value "$properties" ID_SEAT "hermes-kms-$index"
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

if [ "$FAIL" -ne 0 ]; then
	dmesg | tail -n 160 >&2 || true
	exit "$FAIL"
fi

printf 'PASS: one host card and two private session cards were identified without manual device counting\n'
