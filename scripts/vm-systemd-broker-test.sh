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
DIRECT_PID=

cleanup()
{
	[ -n "$DIRECT_PID" ] && kill "$DIRECT_PID" 2>/dev/null || true
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

# A direct invocation used to be refused, because it could have replaced the
# host's /run. The launcher now creates its own mount namespace instead, so the
# property to check is no longer "it says no" but "the host's /run survives".
"$REPO/scripts/hermes-kms-seatd-instance" 8 root root \
	>/dev/null 2>&1 &
DIRECT_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -S /run/hermes-kms-seatd/8/seatd.sock ] && break
	sleep 0.2
done
[ -S /run/hermes-kms-seatd/8/seatd.sock ] || {
	printf 'directly invoked launcher did not expose its socket\n' >&2
	kill "$DIRECT_PID" 2>/dev/null || true
	exit 1
}
# The bind over /run must have stayed inside the launcher's own namespace.
[ -d /run/systemd ] && [ -e /run/udev ] || {
	printf 'a direct invocation replaced the host /run\n' >&2
	kill "$DIRECT_PID" 2>/dev/null || true
	exit 1
}
kill "$DIRECT_PID" 2>/dev/null || true
wait "$DIRECT_PID" 2>/dev/null || true
DIRECT_PID=
rm -rf -- /run/hermes-kms-seatd

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
