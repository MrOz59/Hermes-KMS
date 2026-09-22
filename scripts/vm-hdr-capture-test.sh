#!/usr/bin/env bash
# Only run inside a disposable virtme guest. Build the binary on the host first.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${HERMES_HDR_TEST_BIN:-$REPO/tests/hdr-capture}"
KO="${HERMES_HDR_TEST_MODULE:-$REPO/kernel/hermes-kms/hermes_kms.ko}"
[[ $(id -u) == 0 ]] || { echo 'requires guest root' >&2; exit 1; }
grep -q 'virtme' /proc/cmdline || { echo 'requires a disposable virtme guest' >&2; exit 1; }
[[ ! -d /sys/module/hermes_kms ]] || { echo 'Hermes already loaded; refusing' >&2; exit 1; }
[[ -x "$BIN" ]] || { echo 'build with make check-hdr-build first' >&2; exit 1; }
modprobe drm_shmem_helper
modprobe configfs
insmod "$KO" hdr_enable=1 color_depth=10 hotplug_events=0
trap 'rmmod hermes_kms' EXIT
for node in /sys/class/drm/card[0-9]*; do
 [[ -e "$node/device/driver" ]] || continue
 [[ $(basename "$(readlink "$node/device/driver")") == hermes-kms ]] || continue
 "$BIN" "/dev/dri/$(basename "$node")"
 rmmod hermes_kms
 trap - EXIT
 if dmesg | grep -E 'BUG:|WARNING:|Oops:|KASAN:|possible circular locking'; then
  echo 'kernel diagnostics found' >&2
  exit 1
 fi
 echo 'HDR guest load/capture/unload: PASS'
 exit 0
done
echo 'no Hermes card found' >&2
exit 1
