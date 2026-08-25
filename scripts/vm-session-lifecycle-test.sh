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
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
BIN="$TEST_TMP/hermes-session-lifecycle"
SESSION_FILE="$TEST_TMP/session.auth"
CONTROL_FIFO="$TEST_TMP/control.fifo"
HOLD_PID=
FAIL=0
LOADED_BY_TEST=0

cleanup()
{
	if [ -n "$HOLD_PID" ]; then
		kill -TERM "$HOLD_PID" 2>/dev/null || true
		wait "$HOLD_PID" 2>/dev/null || true
	fi
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

# The same operations through the command-line holder. The owner's
# authorization lives in the descriptor that claimed the output, so a second
# hermes-kmsctl invocation can never rotate or revoke on its behalf; the holder
# reads the commands from its own control FIFO instead.
if [ ! -x "$CTL" ]; then
	printf 'control tool not built, skipping the CLI path: %s\n' "$CTL" >&2
else
	check()
	{
		if [ "$3" != "$2" ]; then
			printf 'FAIL: %s expected %s, got %s\n' \
				"$1" "$2" "${3:-<empty>}" >&2
			FAIL=1
		else
			printf 'ok: %s\n' "$1"
		fi
	}

	"$CTL" --session-file "$SESSION_FILE" --control "$CONTROL_FIFO" \
		hold 1280x720@60 >"$TEST_TMP/hold.log" 2>&1 &
	HOLD_PID=$!
	for _ in $(seq 1 50); do
		[ -s "$SESSION_FILE" ] && [ -p "$CONTROL_FIFO" ] && break
		sleep 0.1
	done
	if [ ! -s "$SESSION_FILE" ] || [ ! -p "$CONTROL_FIFO" ]; then
		printf 'FAIL: holder did not publish its session file and control FIFO\n'
		printf '### holder log:\n'
		cat "$TEST_TMP/hold.log" || true
		printf '### session file: %s control fifo: %s\n' \
			"$(ls -la "$SESSION_FILE" 2>&1)" \
			"$(ls -la "$CONTROL_FIFO" 2>&1)"
		FAIL=1
	else
		printf 'ok: holder published a session file and a control FIFO\n'
		check "control FIFO mode" 600 "$(stat -c %a "$CONTROL_FIFO")"

		cp -- "$SESSION_FILE" "$TEST_TMP/first.auth"
		chmod 0600 "$TEST_TMP/first.auth"
		if "$CTL" --session-file "$SESSION_FILE" status >/dev/null 2>&1; then
			printf 'ok: the published credential binds\n'
		else
			printf 'FAIL: the published credential did not bind\n' >&2
			FAIL=1
		fi

		printf 'rotate\n' > "$CONTROL_FIFO"
		for _ in $(seq 1 50); do
			cmp -s "$TEST_TMP/first.auth" "$SESSION_FILE" || break
			sleep 0.1
		done
		if cmp -s "$TEST_TMP/first.auth" "$SESSION_FILE"; then
			printf 'FAIL: rotate did not republish the session file\n' >&2
			FAIL=1
		else
			printf 'ok: rotate republished the session file\n'
		fi
		cp -- "$SESSION_FILE" "$TEST_TMP/second.auth"
		chmod 0600 "$TEST_TMP/second.auth"
		if "$CTL" --session-file "$TEST_TMP/first.auth" status \
			>/dev/null 2>&1; then
			printf 'FAIL: the retired credential still binds\n' >&2
			FAIL=1
		else
			printf 'ok: the retired credential no longer binds\n'
		fi
		if "$CTL" --session-file "$SESSION_FILE" status >/dev/null 2>&1; then
			printf 'ok: the rotated credential binds\n'
		else
			printf 'FAIL: the rotated credential did not bind\n' >&2
			FAIL=1
		fi

		printf 'revoke\n' > "$CONTROL_FIFO"
		for _ in $(seq 1 50); do
			[ -e "$SESSION_FILE" ] || break
			sleep 0.1
		done
		if [ -e "$SESSION_FILE" ]; then
			printf 'FAIL: revoke left a working credential published\n' >&2
			FAIL=1
		else
			printf 'ok: revoke removed the published credential\n'
		fi
		if "$CTL" --session-file "$TEST_TMP/second.auth" status \
			>/dev/null 2>&1; then
			printf 'FAIL: the pre-revocation credential still binds\n' >&2
			FAIL=1
		else
			printf 'ok: the pre-revocation credential no longer binds\n'
		fi

		# A misplaced option must be reported, not dropped.
		if "$CTL" hold 1280x720@60 --session-file "$TEST_TMP/x.auth" \
			>/dev/null 2>&1; then
			printf 'FAIL: an option after the command was accepted\n' >&2
			FAIL=1
		else
			printf 'ok: an option after the command is rejected\n'
		fi

		# An unknown command must be reported, not acted on.
		printf 'wat\n' > "$CONTROL_FIFO"
		sleep 0.3
		if kill -0 "$HOLD_PID" 2>/dev/null; then
			printf 'ok: the holder survived an unknown command\n'
		else
			printf 'FAIL: the holder exited on an unknown command\n' >&2
			FAIL=1
		fi
	fi

	kill -TERM "$HOLD_PID" 2>/dev/null || true
	wait "$HOLD_PID" 2>/dev/null || true
	HOLD_PID=
	[ -e "$CONTROL_FIFO" ] && {
		printf 'FAIL: the holder left its control FIFO behind\n' >&2
		FAIL=1
	}
	[ -e "$CONTROL_FIFO" ] || printf 'ok: the holder removed its control FIFO\n'
fi

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
