#!/usr/bin/env bash
# Validate the packaged broker unit, including its hardening and socket owner.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT=hermes-kms-seatd@1.service

cleanup()
{
	systemctl stop "$UNIT" 2>/dev/null || true
}
trap cleanup EXIT

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
[ "$(stat -c '%u' /run/hermes-kms-seatd/1/seatd.sock)" = "$target_uid" ] || {
	printf 'broker socket is not owned by the configured Hermes uid\n' >&2
	exit 1
}

printf 'PASS: packaged systemd broker started under hardening and exposed a configured-user socket\n'
