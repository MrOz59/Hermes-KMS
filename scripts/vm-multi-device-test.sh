#!/usr/bin/env bash
# Hermes-KMS independent DRM-master domain smoke test.
#
# Runs in a disposable virtme-ng guest. It loads two Hermes DRM devices with
# one output each, drives both cards concurrently with separate modetest
# processes, and verifies that both capture channels export distinct frames.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
HOLD_PIDS=()
MODETEST_PIDS=()
CARDS=()
FAIL=0

cleanup()
{
	local pid
	local hidden

	for pid in "${MODETEST_PIDS[@]}" "${HOLD_PIDS[@]}"; do
		[ -n "$pid" ] || continue
		kill -TERM "$pid" 2>/dev/null || true
	done
	for pid in "${MODETEST_PIDS[@]}" "${HOLD_PIDS[@]}"; do
		[ -n "$pid" ] || continue
		wait "$pid" 2>/dev/null || true
	done
	for hidden in /dev/dri/card*.hermes-hidden; do
		[ -e "$hidden" ] || continue
		mv "$hidden" "${hidden%.hermes-hidden}"
	done
	timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
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
command -v modetest >/dev/null || {
	printf 'modetest is required\n' >&2
	exit 1
}

insmod "$KO" initial_enabled=0 hotplug_events=0 devices=2 outputs=1
sleep 0.5

[ -d /sys/devices/platform/hermes-kms.0 ] &&
	[ -d /sys/devices/platform/hermes-kms.1 ] || {
	printf 'FAIL: multi-device platform paths are not hermes-kms.0 and .1\n' >&2
	exit 1
}

for card in /dev/dri/card*; do
	[ -e "$card" ] || continue
	if "$CTL" --device "$card" version >/dev/null 2>&1; then
		CARDS+=("$card")
	fi
done

if [ "${#CARDS[@]}" -ne 2 ]; then
	printf 'FAIL: expected two Hermes cards, found %u: %s\n' \
		"${#CARDS[@]}" "${CARDS[*]:-<none>}" >&2
	exit 1
fi

# Sort by the stable UAPI device index rather than the dynamic card number.
if [ "$("$CTL" --device "${CARDS[0]}" identity | value device_index)" != 1 ]; then
	tmp="${CARDS[0]}"
	CARDS[0]="${CARDS[1]}"
	CARDS[1]="$tmp"
fi

for index in 0 1; do
	IDENTITY="$("$CTL" --device "${CARDS[$index]}" identity)"
	CAPS="$("$CTL" --device "${CARDS[$index]}" caps)"
	require_value "$IDENTITY" device_index "$((index + 1))"
	require_value "$IDENTITY" device_count 2
	require_value "$IDENTITY" output_count 1
	require_value "$CAPS" multi_device true
done

"$CTL" --device "${CARDS[0]}" hold 854x480@60 \
	>"/tmp/hermes-device-hold-1.log" 2>&1 &
HOLD_PIDS+=("$!")
"$CTL" --device "${CARDS[1]}" hold 1920x1080@60 \
	>"/tmp/hermes-device-hold-2.log" 2>&1 &
HOLD_PIDS+=("$!")
sleep 0.5

for index in 0 1; do
	for other in "${CARDS[@]}"; do
		[ "$other" = "${CARDS[$index]}" ] && continue
		mv "$other" "$other.hermes-hidden"
	done

	IDENTITY="$("$CTL" --device "${CARDS[$index]}" identity)"
	CONNECTOR="$(printf '%s\n' "$IDENTITY" | value connector_id)"
	CRTC="$(printf '%s\n' "$IDENTITY" | value crtc_id)"
	PLANE="$(printf '%s\n' "$IDENTITY" | value plane_id)"
	if [ "$index" -eq 0 ]; then
		MODE=854x480
	else
		MODE=1920x1080
	fi

	# This libdrm modetest build ignores -D for platform DRM devices and
	# always opens the first matching driver. Temporarily hide the other
	# primary node while modetest opens its fd; moving a devtmpfs node does
	# not affect already-open fds or the driver.
	timeout -k 1s 15s modetest -M hermes-kms -a \
		-s "$CONNECTOR@$CRTC:$MODE@XR24" \
		-P "$PLANE@$CRTC:$MODE@XR24" \
		-v >"/tmp/hermes-device-modetest-$((index + 1)).log" 2>&1 &
	MODETEST_PIDS+=("$!")
	sleep 0.2
	for other in /dev/dri/card*.hermes-hidden; do
		[ -e "$other" ] || continue
		mv "$other" "${other%.hermes-hidden}"
	done
done

for _ in {1..100}; do
	STATUS_1="$("$CTL" --device "${CARDS[0]}" status)"
	STATUS_2="$("$CTL" --device "${CARDS[1]}" status)"
	if [ "$(printf '%s\n' "$STATUS_1" | value scanout_active)" = true ] &&
	   [ "$(printf '%s\n' "$STATUS_2" | value scanout_active)" = true ]; then
		break
	fi
	sleep 0.1
done

require_value "$STATUS_1" scanout_active true
require_value "$STATUS_1" requested 854x480@60
require_value "$STATUS_1" active 854x480@60
require_value "$STATUS_1" framebuffer_size 854x480
require_value "$STATUS_1" framebuffer_plane_0_pitch 3584
require_value "$STATUS_2" scanout_active true
require_value "$STATUS_2" active 1920x1080@60

if [ "$FAIL" -ne 0 ]; then
	for index in 1 2; do
		printf '%s\n' "--- modetest device $index ---" >&2
		sed -n '1,240p' "/tmp/hermes-device-modetest-$index.log" >&2 || true
	done
	printf '%s\n' '--- recent kernel log ---' >&2
	dmesg | tail -n 200 >&2 || true
	exit "$FAIL"
fi

FRAME_1="$("$CTL" --device "${CARDS[0]}" frame --require-dmabuf --sync-file)"
FRAME_2="$("$CTL" --device "${CARDS[1]}" frame --require-dmabuf --sync-file)"
require_value "$FRAME_1" dmabuf_valid true
require_value "$FRAME_1" sync_file_valid true
require_value "$FRAME_1" size 854x480
require_value "$FRAME_1" plane_0_pitch 3584
require_value "$FRAME_2" dmabuf_valid true
require_value "$FRAME_2" sync_file_valid true

FB_1="$(printf '%s\n' "$FRAME_1" | value framebuffer_id)"
FB_2="$(printf '%s\n' "$FRAME_2" | value framebuffer_id)"
if [ -z "$FB_1" ] || [ -z "$FB_2" ]; then
	printf 'FAIL: missing framebuffer id (%s, %s)\n' \
		"${FB_1:-<empty>}" "${FB_2:-<empty>}" >&2
	FAIL=1
fi

if dmesg | grep -iE \
	'BUG:|WARNING:|RIP:|use-after-free|general protection|null pointer'; then
	printf 'FAIL: kernel splat detected\n' >&2
	FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
	exit "$FAIL"
fi

printf 'PASS: two independent Hermes DRM cards accepted simultaneous masters and exported frames\n'
printf 'device_1=%s framebuffer=%s\n' "${CARDS[0]}" "$FB_1"
printf 'device_2=%s framebuffer=%s\n' "${CARDS[1]}" "$FB_2"
