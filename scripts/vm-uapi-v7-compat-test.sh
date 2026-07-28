#!/usr/bin/env bash
# Verify that an unmodified UAPI v7 client still controls only HERMES-1 when
# the current driver exposes multiple outputs.
#
# By default the script builds the old control binary from tag v0.1.2 in a
# temporary directory. A prebuilt path may still be passed explicitly:
#
#   scripts/vm-uapi-v7-compat-test.sh /path/to/hermes-kmsctl-v0.1.2
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
OLD_CTL="${1:-$REPO/tools/hermes-kmsctl/hermes-kmsctl-v0.1.2}"
OLD_BUILD_DIR=""
OLD_HOLD_PID=""
NEW_HOLD_PID=""
FAIL=0

cleanup()
{
	local pid

	for pid in "$OLD_HOLD_PID" "$NEW_HOLD_PID"; do
		[ -n "$pid" ] || continue
		kill -TERM "$pid" 2>/dev/null || true
	done
	for pid in "$OLD_HOLD_PID" "$NEW_HOLD_PID"; do
		[ -n "$pid" ] || continue
		wait "$pid" 2>/dev/null || true
	done
	timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	if [ -n "$OLD_BUILD_DIR" ]; then
		rm -rf -- "$OLD_BUILD_DIR"
	fi
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
if [ ! -x "$OLD_CTL" ] && [ "$#" -eq 0 ]; then
	OLD_BUILD_DIR="$(mktemp -d /tmp/hermes-kms-v0.1.2.XXXXXX)"
	mkdir -p "$OLD_BUILD_DIR/include/drm"
	git -c safe.directory="$REPO" -C "$REPO" \
		show v0.1.2:tools/hermes-kmsctl/hermes_kmsctl.c \
		>"$OLD_BUILD_DIR/hermes_kmsctl.c"
	git -c safe.directory="$REPO" -C "$REPO" \
		show v0.1.2:include/uapi/drm/hermes_kms_drm.h \
		>"$OLD_BUILD_DIR/include/drm/hermes_kms_drm.h"
	"${CC:-cc}" -O2 -Wall -Wextra -I"$OLD_BUILD_DIR/include" \
		-o "$OLD_BUILD_DIR/hermes-kmsctl" "$OLD_BUILD_DIR/hermes_kmsctl.c"
	OLD_CTL="$OLD_BUILD_DIR/hermes-kmsctl"
fi
[ -x "$OLD_CTL" ] || {
	printf 'old v0.1.2 control tool not found: %s\n' "$OLD_CTL" >&2
	exit 1
}

insmod "$KO" initial_enabled=0 hotplug_events=0 outputs=2
sleep 0.5

# These ioctl numbers and struct sizes must remain compatible.
require_value "$("$OLD_CTL" version)" uapi_version 9
require_value "$("$OLD_CTL" identity)" output HERMES-1
"$OLD_CTL" caps >/dev/null
"$OLD_CTL" status >/dev/null

"$OLD_CTL" hold 1920x1080@60 >"/tmp/hermes-old-hold.log" 2>&1 &
OLD_HOLD_PID=$!
sleep 0.5

STATUS_1="$("$CTL" --output 1 status)"
STATUS_2="$("$CTL" --output 2 status)"
require_value "$STATUS_1" enabled true
require_value "$STATUS_2" enabled false

"$CTL" --output 2 hold 1280x720@60 >"/tmp/hermes-new-hold.log" 2>&1 &
NEW_HOLD_PID=$!
sleep 0.5

STATUS_1="$("$CTL" --output 1 status)"
STATUS_2="$("$CTL" --output 2 status)"
require_value "$STATUS_1" enabled true
require_value "$STATUS_2" enabled true

kill -TERM "$OLD_HOLD_PID"
wait "$OLD_HOLD_PID"
OLD_HOLD_PID=""
sleep 0.2

STATUS_1="$("$CTL" --output 1 status)"
STATUS_2="$("$CTL" --output 2 status)"
require_value "$STATUS_1" enabled false
require_value "$STATUS_2" enabled true

if dmesg | grep -iE \
	'BUG:|WARNING:|RIP:|use-after-free|general protection|null pointer'; then
	printf 'FAIL: kernel splat detected\n' >&2
	FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
	exit "$FAIL"
fi

printf '%s\n' \
	'PASS: unmodified v0.1.2 client remains bound to HERMES-1 under UAPI v9'
