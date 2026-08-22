#!/usr/bin/env bash
# Exercise the packaged one-click setup helper in a disposable virtme-ng guest.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
HELPER="$REPO/scripts/hermes-kms-setup"
CONFIG_FILES=(
	/etc/hermes-kms/session-user
	/etc/modprobe.d/hermes-kms.conf
	/etc/modules-load.d/hermes-kms.conf
	/etc/udev/rules.d/90-hermes-kms-user.rules
)
BACKUP_ROOT="$(mktemp -d)"
STUB_ROOT="$(mktemp -d)"
SYSTEMCTL_LOG="$STUB_ROOT/systemctl.log"
FAIL=0
LOADED_BY_TEST=0
CONFIG_DIR_EXISTED=0
RESTORE_CONFIGS=0

cleanup()
{
	if [ "$LOADED_BY_TEST" -eq 1 ]; then
		timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	fi
	if [ "$RESTORE_CONFIGS" -eq 1 ]; then
		for path in "${CONFIG_FILES[@]}"; do
			backup="$BACKUP_ROOT$path"
			rm -rf -- "$path"
			if [ -e "$backup" ] || [ -L "$backup" ]; then
				install -d "$(dirname "$path")"
				cp -a -- "$backup" "$path"
			fi
		done
		[ "$CONFIG_DIR_EXISTED" -eq 1 ] || rmdir /etc/hermes-kms 2>/dev/null || true
	fi
	rm -rf "$BACKUP_ROOT" "$STUB_ROOT"
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root (virtme-ng --exec runs as root by default)\n' >&2
	exit 1
}
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
[ -x "$HELPER" ] || { printf 'setup helper not executable: %s\n' "$HELPER" >&2; exit 1; }
command -v seatd >/dev/null || { printf 'seatd is required in the VM\n' >&2; exit 1; }
[ ! -L /etc/hermes-kms ] || {
	printf 'refusing to test through symlink /etc/hermes-kms\n' >&2
	exit 1
}

[ -d /etc/hermes-kms ] && CONFIG_DIR_EXISTED=1
for path in "${CONFIG_FILES[@]}"; do
	if [ -e "$path" ] || [ -L "$path" ]; then
		install -d "$BACKUP_ROOT$(dirname "$path")"
		cp -a -- "$path" "$BACKUP_ROOT$path"
	fi
done
RESTORE_CONFIGS=1

printf '%s\n' \
	'#!/bin/sh' \
	'printf "%s\n" "$*" >>"$HERMES_TEST_SYSTEMCTL_LOG"' \
	'exit 0' >"$STUB_ROOT/systemctl"
chmod 0755 "$STUB_ROOT/systemctl"

target_uid="$(id -u nobody 2>/dev/null || printf '0')"
DMESG_MARK="$(dmesg | wc -l)"
insmod "$KO" initial_enabled=0 hotplug_events=0 session_devices=2 outputs=1
LOADED_BY_TEST=1

env \
	HERMES_TEST_SYSTEMCTL_LOG="$SYSTEMCTL_LOG" \
	SUDO_UID="$target_uid" \
	PATH="$STUB_ROOT:$PATH" \
	"$HELPER" configure --user auto --session-devices 2

[ "$(cat /etc/hermes-kms/session-user)" = "$target_uid" ] || {
	printf 'FAIL: helper did not persist the target uid\n' >&2
	FAIL=1
}
grep -qx 'options hermes_kms initial_enabled=0 outputs=1 session_devices=2' \
	/etc/modprobe.d/hermes-kms.conf || {
	printf 'FAIL: helper did not persist the requested module topology\n' >&2
	FAIL=1
}
grep -q "OWNER=\"${target_uid}\".*MODE=\"0600\".*TAG-=\"uaccess\"" \
	/etc/udev/rules.d/90-hermes-kms-user.rules || {
	printf 'FAIL: helper did not restrict render nodes to the target uid\n' >&2
	FAIL=1
}
grep -qx 'restart hermes-kms-seatd@1.service' "$SYSTEMCTL_LOG" || {
	printf 'FAIL: broker 1 was not restarted\n' >&2
	FAIL=1
}
grep -qx 'restart hermes-kms-seatd@2.service' "$SYSTEMCTL_LOG" || {
	printf 'FAIL: broker 2 was not restarted\n' >&2
	FAIL=1
}
grep -qx 'disable hermes-kms-seatd@1.service' "$SYSTEMCTL_LOG" &&
	grep -qx 'disable hermes-kms-seatd@8.service' "$SYSTEMCTL_LOG" || {
	printf 'FAIL: legacy manually-enabled brokers were not migrated\n' >&2
	FAIL=1
}
grep -qx 'stop hermes-kms-seatd@3.service' "$SYSTEMCTL_LOG" || {
	printf 'FAIL: stale brokers outside the configured pool were not stopped\n' >&2
	FAIL=1
}
if grep -q '^restart hermes-kms-seatd@3.service$' "$SYSTEMCTL_LOG"; then
	printf 'FAIL: helper restarted a broker outside the configured pool\n' >&2
	FAIL=1
fi

set +e
mismatch_output="$(
	env \
		HERMES_TEST_SYSTEMCTL_LOG="$SYSTEMCTL_LOG" \
		SUDO_UID="$target_uid" \
		PATH="$STUB_ROOT:$PATH" \
		"$HELPER" configure --user auto --session-devices 3 2>&1
)"
mismatch_status=$?
set -e
[ "$mismatch_status" -eq 10 ] || {
	printf 'FAIL: a loaded-topology mismatch returned %s instead of 10\n' \
		"$mismatch_status" >&2
	FAIL=1
}
case "$mismatch_output" in
	*'configuration saved; reboot required'*) ;;
	*)
		printf 'FAIL: topology mismatch did not explain that a reboot is required\n' >&2
		FAIL=1
		;;
esac
grep -qx 'options hermes_kms initial_enabled=0 outputs=1 session_devices=3' \
	/etc/modprobe.d/hermes-kms.conf || {
	printf 'FAIL: reboot-required configuration was not saved for the next load\n' >&2
	FAIL=1
}

if ! timeout -k 1s 5s rmmod hermes_kms; then
	printf 'FAIL: module did not unload cleanly\n' >&2
	FAIL=1
else
	LOADED_BY_TEST=0
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
	exit "$FAIL"
fi

printf 'PASS: one-click setup persisted the user/topology, restarted the exact pool, and handled upgrades safely\n'
