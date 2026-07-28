#!/usr/bin/env bash
# Start two real Weston DRM compositors concurrently, one on each independent
# Hermes-KMS card, inside a disposable virtme-ng guest.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
TEST_ROOT="$(mktemp -d /tmp/hermes-compositors.XXXXXX)"
TEST_USER="${HERMES_TEST_USER:-$(stat -c %U "$REPO")}"
CARDS=()
HOLD_PIDS=()
WESTON_PIDS=()
SEATD_PIDS=()
RUNTIME_RULE="/run/udev/rules.d/70-hermes-kms-session-seats.rules"

cleanup()
{
	local pid

	for pid in "${WESTON_PIDS[@]}" "${SEATD_PIDS[@]}" "${HOLD_PIDS[@]}"; do
		[ -n "$pid" ] || continue
		kill -TERM "$pid" 2>/dev/null || true
	done
	for pid in "${WESTON_PIDS[@]}" "${SEATD_PIDS[@]}" "${HOLD_PIDS[@]}"; do
		[ -n "$pid" ] || continue
		wait "$pid" 2>/dev/null || true
	done
	timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	rm -f -- "$RUNTIME_RULE"
	udevadm control --reload-rules 2>/dev/null || true
	rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

value()
{
	local key="$1"
	awk -F= -v key="$key" '$1 == key { print $2; exit }'
}

fail_with_logs()
{
	local message="$1"
	local log

	printf 'FAIL: %s\n' "$message" >&2
	for log in "$TEST_ROOT"/weston-*/*.log; do
		[ -f "$log" ] || continue
		printf '%s\n' "----- $log -----" >&2
		tail -n 80 "$log" >&2
	done
	exit 1
}

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root (virtme-ng --exec runs as root by default)\n' >&2
	exit 1
}
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
[ -x "$CTL" ] || { printf 'control tool not built: %s\n' "$CTL" >&2; exit 1; }
command -v weston >/dev/null || { printf 'weston is required\n' >&2; exit 1; }
command -v seatd >/dev/null || { printf 'seatd is required in the VM\n' >&2; exit 1; }
command -v runuser >/dev/null || { printf 'runuser is required in the VM\n' >&2; exit 1; }
id "$TEST_USER" >/dev/null 2>&1 ||
	{ printf 'test user does not exist: %s\n' "$TEST_USER" >&2; exit 1; }
if [ "$TEST_USER" != root ] &&
	! id -nG "$TEST_USER" | tr ' ' '\n' | grep -qx seat; then
	printf 'test user %s must belong to the seat group\n' "$TEST_USER" >&2
	exit 1
fi
chmod 0755 "$TEST_ROOT"

mkdir -p "$(dirname "$RUNTIME_RULE")"
cp "$REPO/udev/70-hermes-kms-session-seats.rules" "$RUNTIME_RULE"
udevadm control --reload-rules
insmod "$KO" initial_enabled=0 hotplug_events=0 devices=2 outputs=1
udevadm trigger --subsystem-match=drm --action=change
udevadm settle
sleep 0.5

for card in /dev/dri/card*; do
	[ -e "$card" ] || continue
	if "$CTL" --device "$card" version >/dev/null 2>&1; then
		CARDS+=("$card")
	fi
done
[ "${#CARDS[@]}" -eq 2 ] ||
	fail_with_logs "expected two Hermes cards, found ${#CARDS[@]}"

if [ "$("$CTL" --device "${CARDS[0]}" identity | value device_index)" != 1 ]; then
	tmp="${CARDS[0]}"
	CARDS[0]="${CARDS[1]}"
	CARDS[1]="$tmp"
fi

for index in 0 1; do
	seat="$(udevadm info --query=property --name="${CARDS[$index]}" |
		awk -F= '$1 == "ID_SEAT" { print $2; exit }')"
	[ "$seat" = "hermes-kms-$((index + 1))" ] ||
		fail_with_logs "${CARDS[$index]} has seat ${seat:-<none>}"
done

"$CTL" --device "${CARDS[0]}" hold 854x480@60 \
	>"$TEST_ROOT/hold-1.log" 2>&1 &
HOLD_PIDS+=("$!")
"$CTL" --device "${CARDS[1]}" hold 1920x1080@60 \
	>"$TEST_ROOT/hold-2.log" 2>&1 &
HOLD_PIDS+=("$!")
sleep 0.5

for index in 0 1; do
	runtime="$TEST_ROOT/weston-$((index + 1))"
	seatd_runtime="$TEST_ROOT/seatd-runtime/$((index + 1))"
	install -d -m 0700 -o "$TEST_USER" -g "$(id -gn "$TEST_USER")" "$runtime"
	HERMES_SEATD_RUNTIME_ROOT="$TEST_ROOT/seatd-runtime" \
	unshare --mount --propagation private \
		"$REPO/scripts/hermes-kms-seatd-instance" \
		"$((index + 1))" root seat >"$runtime/seatd.log" 2>&1 &
	SEATD_PIDS+=("$!")
	for attempt in $(seq 1 100); do
		[ -S "$seatd_runtime/seatd.sock" ] && break
		kill -0 "${SEATD_PIDS[$index]}" 2>/dev/null ||
			fail_with_logs "seatd instance $((index + 1)) exited before creating its socket"
		sleep 0.02
	done
	[ -S "$seatd_runtime/seatd.sock" ] ||
		fail_with_logs "seatd instance $((index + 1)) did not create its socket"
	runuser -u "$TEST_USER" -- test -r "$seatd_runtime/seatd.sock" ||
		fail_with_logs "$TEST_USER cannot read seatd instance $((index + 1)) socket"
	runuser -u "$TEST_USER" -- test -w "$seatd_runtime/seatd.sock" ||
		fail_with_logs "$TEST_USER cannot write seatd instance $((index + 1)) socket"

	runuser -u "$TEST_USER" -- env \
		SEATD_SOCK="$seatd_runtime/seatd.sock" \
		XDG_RUNTIME_DIR="$runtime" \
		XDG_SEAT="hermes-kms-$((index + 1))" \
		LIBSEAT_BACKEND=seatd \
		weston \
		--backend=drm \
		--renderer=pixman \
		--drm-device="$(basename "${CARDS[$index]}")" \
		--seat="hermes-kms-$((index + 1))" \
		--continue-without-input \
		--socket="wayland-$index" \
		--shell=desktop \
		--idle-time=0 \
		--no-config \
		--log="$runtime/weston.log" \
		>"$runtime/launcher.log" 2>&1 &
	WESTON_PIDS+=("$!")
done

for attempt in $(seq 1 100); do
	ready=0
	for index in 0 1; do
		status="$("$CTL" --device "${CARDS[$index]}" status)"
		[ "$(printf '%s\n' "$status" | value frame_valid)" = true ] &&
			[ "$(printf '%s\n' "$status" | value scanout_active)" = true ] &&
			ready=$((ready + 1))
	done
	[ "$ready" -eq 2 ] && break
	for pid in "${WESTON_PIDS[@]}"; do
		kill -0 "$pid" 2>/dev/null ||
			fail_with_logs "Weston exited before both scanouts became ready"
	done
	sleep 0.05
done
[ "${ready:-0}" -eq 2 ] ||
	fail_with_logs "timed out waiting for two Weston scanouts"

STATUS_1="$("$CTL" --device "${CARDS[0]}" status)"
STATUS_2="$("$CTL" --device "${CARDS[1]}" status)"
[ "$(printf '%s\n' "$STATUS_1" | value active)" = "854x480@60" ] ||
	fail_with_logs "first compositor did not scan out at 854x480@60"
[ "$(printf '%s\n' "$STATUS_2" | value active)" = "1920x1080@60" ] ||
	fail_with_logs "second compositor did not scan out at 1920x1080@60"

if dmesg | grep -iE \
	'BUG:|WARNING:|RIP:|use-after-free|general protection|null pointer'; then
	fail_with_logs "kernel splat detected"
fi

printf '%s\n' \
	"PASS: two Weston compositors scanned out concurrently on independent Hermes cards" \
	"device_1=${CARDS[0]} active=854x480@60" \
	"device_2=${CARDS[1]} active=1920x1080@60"
