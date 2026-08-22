#!/usr/bin/env bash
# Validate the packaged broker unit, including its hardening and socket owner.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT=hermes-kms-seatd@1.service
TARGET_FILES=(
	/usr/lib/hermes-kms/hermes-kms-seatd-instance
	/run/systemd/system/hermes-kms-seatd@.service
	/etc/hermes-kms/session-user
)
BACKUP_ROOT=
CONFIG_DIR_EXISTED=0

cleanup()
{
	systemctl stop "$UNIT" 2>/dev/null || true
	rmdir /run/hermes-kms-seatd 2>/dev/null || true
	if [ -n "$BACKUP_ROOT" ]; then
		for path in "${TARGET_FILES[@]}"; do
			rm -rf -- "$path"
			backup="$BACKUP_ROOT$path"
			if [ -e "$backup" ] || [ -L "$backup" ]; then
				install -d "$(dirname "$path")"
				cp -a -- "$backup" "$path"
			fi
		done
		[ "$CONFIG_DIR_EXISTED" -eq 1 ] || rmdir /etc/hermes-kms 2>/dev/null || true
		systemctl daemon-reload 2>/dev/null || true
		rm -rf -- "$BACKUP_ROOT"
	fi
}

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root in a virtme-ng --systemd guest\n' >&2
	exit 1
}
[ -d /run/systemd/system ] || {
	printf 'this test requires systemd as the guest init\n' >&2
	exit 1
}
command -v seatd >/dev/null || {
	printf 'seatd is required in the VM\n' >&2
	exit 1
}
[ ! -L /etc/hermes-kms ] || {
	printf 'refusing to test through symlink /etc/hermes-kms\n' >&2
	exit 1
}
[ ! -e /run/hermes-kms-seatd ] && [ ! -L /run/hermes-kms-seatd ] || {
	printf 'refusing to replace an existing /run/hermes-kms-seatd test target\n' >&2
	exit 1
}

if direct_output="$("$REPO/scripts/hermes-kms-seatd-instance" 1 root root 2>&1)"; then
	printf 'launcher accepted a dangerous direct host-namespace invocation\n' >&2
	exit 1
fi
printf '%s\n' "$direct_output" | \
	grep -q 'refusing to run in the host mount namespace' || {
	printf 'launcher did not explain its host-namespace refusal\n' >&2
	exit 1
}

[ -d /etc/hermes-kms ] && CONFIG_DIR_EXISTED=1
BACKUP_ROOT="$(mktemp -d /tmp/hermes-systemd-broker.XXXXXX)"
for path in "${TARGET_FILES[@]}"; do
	if [ -e "$path" ] || [ -L "$path" ]; then
		install -d "$BACKUP_ROOT$(dirname "$path")"
		cp -a -- "$path" "$BACKUP_ROOT$path"
	fi
done
trap cleanup EXIT

target_uid="$(id -u nobody)"
install -Dm0755 "$REPO/scripts/hermes-kms-seatd-instance" \
	/usr/lib/hermes-kms/hermes-kms-seatd-instance
install -Dm0644 "$REPO/packaging/systemd/hermes-kms-seatd@.service" \
	/run/systemd/system/hermes-kms-seatd@.service
install -Dm0644 /dev/null /etc/hermes-kms/session-user
printf '%s\n' "$target_uid" >/etc/hermes-kms/session-user

systemctl daemon-reload
systemctl start "$UNIT"

for _ in 1 2 3 4 5; do
	[ -S /run/hermes-kms-seatd/1/seatd.sock ] && break
	sleep 0.2
done

if ! systemctl --quiet is-active "$UNIT"; then
	systemctl status "$UNIT" --no-pager >&2 || true
	journalctl -u "$UNIT" -n 80 --no-pager >&2 || true
	exit 1
fi
[ -S /run/hermes-kms-seatd/1/seatd.sock ] || {
	printf 'broker did not create its host-visible socket\n' >&2
	exit 1
}
[ "$(stat -c '%u:%g:%a' /run/hermes-kms-seatd)" = '0:0:711' ] || {
	printf 'common runtime directory is not root-owned mode 0711\n' >&2
	exit 1
}
[ "$(stat -c '%u:%g:%a' /run/hermes-kms-seatd/1)" = '0:0:711' ] || {
	printf 'instance runtime directory is not root-owned mode 0711\n' >&2
	exit 1
}
[ "$(stat -c '%u' /run/hermes-kms-seatd/1/seatd.sock)" = "$target_uid" ] || {
	printf 'broker socket is not owned by the configured Hermes uid\n' >&2
	exit 1
}
[ "$(stat -c '%g' /run/hermes-kms-seatd/1/seatd.sock)" = 0 ] || {
	printf 'broker socket group unexpectedly grants non-root access\n' >&2
	exit 1
}

printf 'PASS: packaged systemd broker started under hardening and exposed a configured-user socket\n'
