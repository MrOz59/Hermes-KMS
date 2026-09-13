#!/usr/bin/env bash
# Check host access and unload with a compositor-like DRM fd in a disposable VM.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
TMP="$(mktemp -d)"
HOLDER=

cleanup()
{
	if [ -d /sys/kernel/config/hermes-kms/unload-guard ]; then
		rmdir /sys/kernel/config/hermes-kms/unload-guard 2>/dev/null || true
	fi
	if [ -n "$HOLDER" ]; then
		kill "$HOLDER" 2>/dev/null || true
		wait "$HOLDER" 2>/dev/null || true
	fi
	if [ -d /sys/module/hermes_kms ]; then
		"$REPO/scripts/hermes-kms-unload" >/dev/null 2>&1 || true
	fi
	rm -rf -- "$TMP"
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || { printf 'run in a virtme-ng guest as root\n' >&2; exit 1; }
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
[ ! -d /sys/module/hermes_kms ] || { printf 'module already loaded\n' >&2; exit 1; }

make -C "$REPO" install-runtime-udev >"$TMP/install.log" 2>&1 || {
	cat "$TMP/install.log" >&2
	exit 1
}
insmod "$KO" initial_enabled=0 outputs=1 session_devices=0
udevadm settle

sysnode=(/sys/devices/platform/hermes-kms*/drm/renderD*)
[ -e "${sysnode[0]}" ] || { printf 'host render node was not created\n' >&2; exit 1; }
platform=${sysnode[0]%/drm/*}
render="/dev/dri/${sysnode[0]##*/}"
card_sysnode=("$platform"/drm/card[0-9]*)
card="/dev/dri/${card_sysnode[0]##*/}"

[ "$(stat -c '%U:%G' "$render")" = root:root ] || {
	printf 'host render node lost root ownership\n' >&2
	exit 1
}
udevadm info --query=property --name="$render" |
	grep -Eq '^(TAGS|CURRENT_TAGS)=.*uaccess' || {
	printf 'host render node has no uaccess tag\n' >&2
	exit 1
}
printf 'ok: root-owned host render node receives uaccess\n'

mkdir /sys/kernel/config/hermes-kms/unload-guard
if "$REPO/scripts/hermes-kms-unload" >"$TMP/configfs-guard" 2>&1; then
	printf 'unload ignored a configfs group\n' >&2
	exit 1
fi
[ -L "$platform/driver" ] || {
	printf 'unload detached the static card despite a configfs group\n' >&2
	exit 1
}
rmdir /sys/kernel/config/hermes-kms/unload-guard
printf 'ok: configfs-owned cards prevent detaching unrelated static cards\n'

python3 - "$card" "$TMP/ready" <<'PY' &
import os
import sys
import time
fd = os.open(sys.argv[1], os.O_RDWR)
open(sys.argv[2], 'w').close()
time.sleep(30)
os.close(fd)
PY
HOLDER=$!
for _ in {1..30}; do
	[ -f "$TMP/ready" ] && break
	sleep 0.1
done
[ -f "$TMP/ready" ] || { printf 'test fd holder did not start\n' >&2; exit 1; }

if "$REPO/scripts/hermes-kms-unload" >"$TMP/first-unload" 2>&1; then
	printf 'module unloaded while a DRM fd was still open\n' >&2
	exit 1
fi
[ -d /sys/module/hermes_kms ] || { printf 'module disappeared despite open fd\n' >&2; exit 1; }
[ ! -L "$platform/driver" ] || {
	printf 'unload did not detach the static card\n' >&2
	exit 1
}
grep -q 'still in use' "$TMP/first-unload" || {
	printf 'unload did not explain the remaining reference\n' >&2
	exit 1
}
grep -q "$HOLDER" "$TMP/first-unload" || {
	printf 'unload did not identify the open DRM fd holder\n' >&2
	exit 1
}
printf 'ok: busy module leaves its DRM card detached and reports the blocker\n'

"$REPO/scripts/hermes-kms-rebind" >/dev/null
[ -L "$platform/driver" ] || {
	printf 'rebind did not restore the static card\n' >&2
	exit 1
}
printf 'ok: detached card can be restored\n'

kill "$HOLDER"
wait "$HOLDER" 2>/dev/null || true
HOLDER=
"$REPO/scripts/hermes-kms-unload" >/dev/null
[ ! -d /sys/module/hermes_kms ] || { printf 'module remained loaded\n' >&2; exit 1; }
printf 'ok: unload succeeds after the external DRM fd closes\n'

insmod "$KO" initial_enabled=0 outputs=1 session_devices=2
udevadm settle
pool_host=(/sys/devices/platform/hermes-kms.0/drm/renderD*)
pool_private=(/sys/devices/platform/hermes-kms.1/drm/renderD*)
udevadm info --query=property --name="/dev/dri/${pool_host[0]##*/}" |
	grep -Eq '^(TAGS|CURRENT_TAGS)=.*uaccess' || {
	printf 'pool host render node has no uaccess tag\n' >&2
	exit 1
}
if udevadm info --query=property --name="/dev/dri/${pool_private[0]##*/}" |
	grep -Eq '^(TAGS|CURRENT_TAGS)=.*uaccess'; then
	printf 'private pool render node gained uaccess\n' >&2
	exit 1
fi
"$REPO/scripts/hermes-kms-unload" >/dev/null
[ ! -d /sys/module/hermes_kms ] || { printf 'pool module remained loaded\n' >&2; exit 1; }
printf 'PASS: unload detaches a single host card or a host/private pool\n'
