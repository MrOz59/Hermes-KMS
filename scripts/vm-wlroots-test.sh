#!/usr/bin/env bash
# Exercise a wlroots DRM compositor on a private virtual card in virtme-ng.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
TEST_USER="${HERMES_TEST_USER:-$(stat -c %U "$REPO")}"
TEST_ROOT="$(mktemp -d /tmp/hermes-wlroots.XXXXXX)"
RULE=/run/udev/rules.d/72-hermes-kms-session-seats.rules
RULE_BACKUP=
HOLD_PID=
SEATD_PID=
LABWC_PID=
MODULE_LOADED=0

cleanup()
{
	for pid in "$LABWC_PID" "$SEATD_PID" "$HOLD_PID"; do
		[ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
	done
	for pid in "$LABWC_PID" "$SEATD_PID" "$HOLD_PID"; do
		[ -n "$pid" ] && wait "$pid" 2>/dev/null || true
	done
	if [ "$MODULE_LOADED" -eq 1 ]; then
		timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	fi
	if [ -n "$RULE_BACKUP" ]; then
		cp -- "$RULE_BACKUP" "$RULE"
		unlink "$RULE_BACKUP"
	else
		unlink "$RULE" 2>/dev/null || true
	fi
	udevadm control --reload-rules 2>/dev/null || true
	rm -r -- "$TEST_ROOT"
}
trap cleanup EXIT

value() { awk -F= -v key="$1" '$1 == key { print $2; exit }'; }
fail()
{
	printf 'FAIL: %s\n' "$1" >&2
	for log in "$TEST_ROOT"/*.log; do
		[ -f "$log" ] && { printf '%s\n' "--- $log" >&2; tail -n 90 "$log" >&2; }
	done
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail 'run in a root virtme-ng guest'
[ -f "$KO" ] && [ -x "$CTL" ] || fail 'build the module and control tool first'
command -v labwc >/dev/null || fail 'labwc is required'
command -v seatd >/dev/null || fail 'seatd is required'
id "$TEST_USER" >/dev/null || fail "test user $TEST_USER does not exist"
[ ! -d /sys/module/hermes_kms ] || fail 'module is already loaded'

chmod 0755 "$TEST_ROOT"
printf '%s\n' "$(id -u "$TEST_USER")" > "$TEST_ROOT/session-user"
install -d -m 0700 -o "$TEST_USER" -g "$(id -gn "$TEST_USER")" \
	"$TEST_ROOT/runtime"
if [ -e "$RULE" ]; then
	RULE_BACKUP="$(mktemp /tmp/hermes-wlroots-rule.XXXXXX)"
	cp -- "$RULE" "$RULE_BACKUP"
fi
install -Dm0644 "$REPO/udev/72-hermes-kms-session-seats.rules" "$RULE"
udevadm control --reload-rules
insmod "$KO" initial_enabled=0 session_devices=1 outputs=1
MODULE_LOADED=1
udevadm trigger --subsystem-match=drm --action=change
udevadm settle

CARD=
for card in /dev/dri/card*; do
	[ -e "$card" ] || continue
	if [ "$("$CTL" --device "$card" identity 2>/dev/null |
		value device_role)" = session ]; then
		CARD="$card"
		break
	fi
done
[ -n "$CARD" ] || fail 'no private card was created'

"$CTL" --device "$CARD" --session-file "$TEST_ROOT/owner.auth" \
	hold 1280x720@60 > "$TEST_ROOT/hold.log" 2>&1 &
HOLD_PID=$!
for _ in $(seq 1 100); do
	[ -f "$TEST_ROOT/owner.auth" ] && break
	sleep 0.02
done
[ -f "$TEST_ROOT/owner.auth" ] || fail 'owner did not connect the output'

HERMES_SEATD_RUNTIME_ROOT="$TEST_ROOT/seatd-runtime" \
HERMES_SESSION_USER_FILE="$TEST_ROOT/session-user" \
	"$REPO/scripts/hermes-kms-seatd-instance" 1 auto root \
	> "$TEST_ROOT/seatd.log" 2>&1 &
SEATD_PID=$!
for _ in $(seq 1 100); do
	[ -S "$TEST_ROOT/seatd-runtime/1/seatd.sock" ] && break
	sleep 0.02
done
[ -S "$TEST_ROOT/seatd-runtime/1/seatd.sock" ] || fail 'seatd socket missing'

# An explicit card bypasses wlroots' seat-name enumeration, and omitting the
# libinput backend keeps host seat0 input devices out of the private session.
runuser -u "$TEST_USER" -- env \
	SEATD_SOCK="$TEST_ROOT/seatd-runtime/1/seatd.sock" \
	LIBSEAT_BACKEND=seatd \
	XDG_RUNTIME_DIR="$TEST_ROOT/runtime" \
	WLR_BACKENDS=drm \
	WLR_DRM_DEVICES="$CARD" \
	WLR_LIBINPUT_NO_DEVICES=1 \
	WLR_RENDERER=pixman \
	labwc -d > "$TEST_ROOT/labwc.log" 2>&1 &
LABWC_PID=$!

for _ in $(seq 1 100); do
	status="$("$CTL" --device "$CARD" --session-file "$TEST_ROOT/owner.auth" status)"
	if [ "$(printf '%s\n' "$status" | value frame_valid)" = true ] &&
	   [ "$(printf '%s\n' "$status" | value scanout_active)" = true ]; then
		printf 'PASS: labwc scanned out on private card %s\n' "$CARD"
		exit 0
	fi
	kill -0 "$LABWC_PID" 2>/dev/null || fail 'labwc exited before scanout'
	sleep 0.05
done
fail 'labwc did not scan out within five seconds'
