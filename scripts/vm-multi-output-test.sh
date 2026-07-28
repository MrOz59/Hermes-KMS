#!/usr/bin/env bash
# Hermes-KMS multi-output topology, ownership and frame-isolation smoke test.
#
# Intended for a disposable virtme-ng guest:
#
#   virtme-ng --run --cwd "$PWD" --exec \
#     "bash scripts/vm-multi-output-test.sh"
#
# It never needs a physical GPU or compositor. modetest drives two independent
# atomic pipelines on the Hermes card and hermes-kmsctl verifies that each
# selected fd sees its own owner, mode and framebuffer.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
HOLD_1_PID=""
HOLD_2_PID=""
MODETEST_PID=""
STATUS_1=""
STATUS_2=""
FAIL=0

cleanup()
{
	local pid

	for pid in "$MODETEST_PID" "$HOLD_1_PID" "$HOLD_2_PID"; do
		[ -n "$pid" ] || continue
		kill -TERM "$pid" 2>/dev/null || true
	done
	sleep 0.2
	for pid in "$MODETEST_PID" "$HOLD_1_PID" "$HOLD_2_PID"; do
		[ -n "$pid" ] || continue
		kill -KILL "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
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

dump_diagnostics()
{
	printf '%s\n' '--- modetest log ---' >&2
	sed -n '1,240p' "/tmp/hermes-modetest.log" >&2 || true
	if [ -n "$MODETEST_PID" ] && [ -r "/proc/$MODETEST_PID/stack" ]; then
		printf '%s\n' '--- modetest kernel stack ---' >&2
		cat "/proc/$MODETEST_PID/stack" >&2 || true
	fi
	printf '%s\n' '--- recent kernel log ---' >&2
	dmesg | tail -n 160 >&2 || true
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

insmod "$KO" initial_enabled=0 hotplug_events=0 outputs=2
sleep 0.5

VERSION="$("$CTL" version)"
CAPS="$("$CTL" caps)"
require_value "$VERSION" uapi_version 8
require_value "$CAPS" output_count 2
require_value "$CAPS" multi_output true

"$CTL" --output 1 hold 1920x1080@60 >"/tmp/hermes-hold-1.log" 2>&1 &
HOLD_1_PID=$!
"$CTL" --output 2 hold 1280x720@60 >"/tmp/hermes-hold-2.log" 2>&1 &
HOLD_2_PID=$!
sleep 1

OUTPUTS="$("$CTL" outputs)"
require_value "$OUTPUTS" output_1_enabled true
require_value "$OUTPUTS" output_2_enabled true

IDENTITY_1="$("$CTL" --output 1 identity)"
IDENTITY_2="$("$CTL" --output 2 identity)"
CONNECTOR_1="$(printf '%s\n' "$IDENTITY_1" | value connector_id)"
CRTC_1="$(printf '%s\n' "$IDENTITY_1" | value crtc_id)"
PLANE_1="$(printf '%s\n' "$IDENTITY_1" | value plane_id)"
CONNECTOR_2="$(printf '%s\n' "$IDENTITY_2" | value connector_id)"
CRTC_2="$(printf '%s\n' "$IDENTITY_2" | value crtc_id)"
PLANE_2="$(printf '%s\n' "$IDENTITY_2" | value plane_id)"

timeout -k 1s 20s modetest -M hermes-kms -a \
	-s "$CONNECTOR_1@$CRTC_1:1920x1080@XR24" \
	-P "$PLANE_1@$CRTC_1:1920x1080@XR24" \
	-s "$CONNECTOR_2@$CRTC_2:1280x720@XR24" \
	-P "$PLANE_2@$CRTC_2:1280x720@XR24" \
	-v >"/tmp/hermes-modetest.log" 2>&1 &
MODETEST_PID=$!

for _ in {1..100}; do
	if ! kill -0 "$MODETEST_PID" 2>/dev/null; then
		break
	fi
	STATUS_1="$("$CTL" --output 1 status)"
	STATUS_2="$("$CTL" --output 2 status)"
	if [ "$(printf '%s\n' "$STATUS_1" | value scanout_active)" = true ] &&
	   [ "$(printf '%s\n' "$STATUS_2" | value scanout_active)" = true ]; then
		break
	fi
	sleep 0.1
done

if ! kill -0 "$MODETEST_PID" 2>/dev/null ||
   [ "$(printf '%s\n' "$STATUS_1" | value scanout_active)" != true ] ||
   [ "$(printf '%s\n' "$STATUS_2" | value scanout_active)" != true ]; then
	printf 'FAIL: both outputs did not reach active scanout before timeout\n' >&2
	printf '%s\n' "$STATUS_1" "$STATUS_2" >&2
	dump_diagnostics
	exit 1
fi

if ! FRAME_1="$("$CTL" --output 1 frame --require-dmabuf --sync-file 2>&1)"; then
	printf 'FAIL: output 1 frame acquisition failed: %s\n' "$FRAME_1" >&2
	printf '%s\n' "$STATUS_1" >&2
	dump_diagnostics
	exit 1
fi
if ! FRAME_2="$("$CTL" --output 2 frame --require-dmabuf --sync-file 2>&1)"; then
	printf 'FAIL: output 2 frame acquisition failed: %s\n' "$FRAME_2" >&2
	printf '%s\n' "$STATUS_2" >&2
	dump_diagnostics
	exit 1
fi

require_value "$STATUS_1" scanout_active true
require_value "$STATUS_1" active 1920x1080@60
require_value "$STATUS_2" scanout_active true
require_value "$STATUS_2" active 1280x720@60
require_value "$FRAME_1" dmabuf_valid true
require_value "$FRAME_1" sync_file_valid true
require_value "$FRAME_2" dmabuf_valid true
require_value "$FRAME_2" sync_file_valid true

FRAMEBUFFER_1="$(printf '%s\n' "$FRAME_1" | value framebuffer_id)"
FRAMEBUFFER_2="$(printf '%s\n' "$FRAME_2" | value framebuffer_id)"
if [ -z "$FRAMEBUFFER_1" ] || [ -z "$FRAMEBUFFER_2" ] ||
   [ "$FRAMEBUFFER_1" = "$FRAMEBUFFER_2" ]; then
	printf 'FAIL: expected distinct framebuffers, got %s and %s\n' \
		"${FRAMEBUFFER_1:-<empty>}" "${FRAMEBUFFER_2:-<empty>}" >&2
	FAIL=1
fi

kill -TERM "$HOLD_1_PID"
wait "$HOLD_1_PID"
HOLD_1_PID=""
sleep 0.5
OUTPUTS="$("$CTL" outputs)"
require_value "$OUTPUTS" output_1_enabled false
require_value "$OUTPUTS" output_2_enabled true

if dmesg | grep -iE \
	'BUG:|WARNING:|RIP:|use-after-free|general protection|null pointer'; then
	printf 'FAIL: kernel splat detected\n' >&2
	FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
	exit "$FAIL"
fi

printf 'PASS: two outputs have independent owners, modes and DMA-BUF framebuffers\n'
printf 'output_1_framebuffer=%s\n' "$FRAMEBUFFER_1"
printf 'output_2_framebuffer=%s\n' "$FRAMEBUFFER_2"
