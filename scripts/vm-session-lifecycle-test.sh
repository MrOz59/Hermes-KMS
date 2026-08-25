#!/usr/bin/env bash
# Hermes-KMS session capability lifecycle test.
#
# Runs in a disposable virtme-ng guest. It drives hermes-session-lifecycle,
# which holds an output as owner plus several capture descriptors in one
# process and checks that token rotation, binding revocation, binding
# accounting and blocked-wait wakeup behave as the UAPI promises.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
SRC="$REPO/scripts/hermes-session-lifecycle.c"
TEST_TMP="$(mktemp -d /tmp/hermes-session-lifecycle.XXXXXX)"
BIN="$TEST_TMP/hermes-session-lifecycle"
FAIL=0
LOADED_BY_TEST=0

cleanup()
{
	if [ "$LOADED_BY_TEST" -eq 1 ]; then
		timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	fi
	rm -rf -- "$TEST_TMP"
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root (virtme-ng --exec runs as root by default)\n' >&2
	exit 1
}
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
[ ! -d /sys/module/hermes_kms ] || {
	printf 'hermes_kms is already loaded; use a disposable VM or unload it first\n' >&2
	exit 1
}

if ! cc -O2 -pthread -I"$REPO/include/uapi" \
	$(pkg-config --cflags libdrm) -o "$BIN" "$SRC" -ldrm \
	>"$TEST_TMP/build.log" 2>&1; then
	printf '### could not build hermes-session-lifecycle:\n' >&2
	cat "$TEST_TMP/build.log" >&2
	exit 1
fi

DMESG_MARK="$(dmesg | wc -l)"
insmod "$KO" initial_enabled=0 hotplug_events=0
LOADED_BY_TEST=1
sleep 0.5

"$BIN" || FAIL=1

SPLAT="$(dmesg | tail -n +"$((DMESG_MARK + 1))" |
	grep -iE 'BUG:|WARNING:|use-after-free|general protection' || true)"
if [ -n "$SPLAT" ]; then
	printf '### kernel splat during the run:\n%s\n' "$SPLAT" >&2
	FAIL=1
fi

rmmod hermes_kms
LOADED_BY_TEST=0

if [ "$FAIL" -eq 0 ]; then
	printf '\nPASS: session capability lifecycle\n'
else
	printf '\nFAIL: session capability lifecycle\n' >&2
fi
exit "$FAIL"
