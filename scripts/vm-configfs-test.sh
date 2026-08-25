#!/usr/bin/env bash
# Hermes-KMS runtime card creation smoke test.
#
# Runs in a disposable virtme-ng guest. It creates and removes Hermes DRM cards
# through configfs while a statically configured card stays loaded, and checks
# that identity, seat metadata, output naming and teardown all hold up.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
CTL="$REPO/tools/hermes-kmsctl/hermes-kmsctl"
CONFIGFS=/sys/kernel/config/hermes-kms
FAIL=0
LOADED_BY_TEST=0

cleanup()
{
	local group

	for group in "$CONFIGFS"/*; do
		[ -d "$group" ] || continue
		printf 0 > "$group/enabled" 2>/dev/null || true
		rmdir "$group" 2>/dev/null || true
	done
	if [ "$LOADED_BY_TEST" -eq 1 ]; then
		timeout -k 1s 5s rmmod hermes_kms 2>/dev/null || true
	fi
}
trap cleanup EXIT

value()
{
	local key="$1"
	awk -F= -v key="$key" '$1 == key { print $2; exit }'
}

check()
{
	local what="$1"
	local expected="$2"
	local actual="$3"

	if [ "$actual" != "$expected" ]; then
		printf 'FAIL: %s expected %s, got %s\n' \
			"$what" "$expected" "${actual:-<empty>}" >&2
		FAIL=1
	else
		printf 'ok: %s = %s\n' "$what" "$actual"
	fi
}

# Expect a write to be rejected. A silently accepted write is the failure this
# guards against: the KMS object graph is built once, at probe.
refuse()
{
	local what="$1"
	local path="$2"
	local text="$3"

	if printf '%s' "$text" > "$path" 2>/dev/null; then
		printf 'FAIL: %s was accepted while the card is live\n' "$what" >&2
		FAIL=1
	else
		printf 'ok: %s rejected while the card is live\n' "$what"
	fi
}

hermes_cards()
{
	local card
	local count=0

	for card in /dev/dri/card*; do
		[ -e "$card" ] || continue
		if "$CTL" --device "$card" version >/dev/null 2>&1; then
			count=$((count + 1))
		fi
	done
	printf '%u' "$count"
}

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root (virtme-ng --exec runs as root by default)\n' >&2
	exit 1
}
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
[ -x "$CTL" ] || { printf 'control tool not built: %s\n' "$CTL" >&2; exit 1; }
[ -d /sys/kernel/config ] || {
	printf 'configfs is not mounted at /sys/kernel/config\n' >&2
	exit 1
}
[ ! -d /sys/module/hermes_kms ] || {
	printf 'hermes_kms is already loaded; use a disposable VM or unload it first\n' >&2
	exit 1
}

insmod "$KO" initial_enabled=0 hotplug_events=0
LOADED_BY_TEST=1
sleep 0.5

[ -d "$CONFIGFS" ] || {
	printf 'FAIL: %s was not created\n' "$CONFIGFS" >&2
	exit 1
}
check "static card count" 1 "$(hermes_cards)"

mkdir "$CONFIGFS/stream1"
check "default outputs" 1 "$(cat "$CONFIGFS/stream1/outputs")"
check "default role" general "$(cat "$CONFIGFS/stream1/role")"
check "default enabled" 0 "$(cat "$CONFIGFS/stream1/enabled")"
check "unenabled device_index" -1 "$(cat "$CONFIGFS/stream1/device_index")"
check "unenabled card" "" "$(cat "$CONFIGFS/stream1/card")"

# A session card with no index would land on no udev seat and no broker.
printf session > "$CONFIGFS/stream1/role"
if printf 1 > "$CONFIGFS/stream1/enabled" 2>/dev/null; then
	printf 'FAIL: a session card with index 0 was accepted\n' >&2
	FAIL=1
	printf 0 > "$CONFIGFS/stream1/enabled"
else
	printf 'ok: session role without an index is rejected\n'
fi

printf 2 > "$CONFIGFS/stream1/outputs"
printf 3 > "$CONFIGFS/stream1/session_index"
printf 1 > "$CONFIGFS/stream1/enabled"
sleep 0.5

check "live card count" 2 "$(hermes_cards)"
CARD="$(cat "$CONFIGFS/stream1/card")"
RENDER="$(cat "$CONFIGFS/stream1/render_node")"
[ -e "/dev/dri/$CARD" ] || { printf 'FAIL: %s does not exist\n' "$CARD" >&2; FAIL=1; }
[ -e "/dev/dri/$RENDER" ] || {
	printf 'FAIL: %s does not exist\n' "$RENDER" >&2
	FAIL=1
}

IDENTITY="$("$CTL" --device "/dev/dri/$CARD" identity)"
check "identity device_role" session \
	"$(printf '%s\n' "$IDENTITY" | value device_role)"
check "identity session_index" 3 \
	"$(printf '%s\n' "$IDENTITY" | value session_index)"
check "identity output_count" 2 \
	"$(printf '%s\n' "$IDENTITY" | value output_count)"
check "identity device_count" 2 \
	"$(printf '%s\n' "$IDENTITY" | value device_count)"

# udev keys its seat and broker rules off these, so they must match the request.
PLATFORM="/sys/devices/platform/hermes-kms.$(cat "$CONFIGFS/stream1/device_index")"
check "sysfs role" session "$(cat "$PLATFORM/hermes_kms_role")"
check "sysfs session_index" 3 "$(cat "$PLATFORM/hermes_kms_session_index")"

# Output names double as EDID serials, so they must stay unique across cards
# that are created and removed independently.
NAMES=""
NAME_COUNT=0
for card in /dev/dri/card*; do
	[ -e "$card" ] || continue
	"$CTL" --device "$card" version >/dev/null 2>&1 || continue
	name="$("$CTL" --device "$card" identity | value output)"
	NAMES="$NAMES $name"
	NAME_COUNT=$((NAME_COUNT + 1))
done
UNIQUE_COUNT="$(printf '%s\n' $NAMES | sort -u | wc -l)"
check "distinct output names across cards ($NAMES )" \
	"$NAME_COUNT" "$UNIQUE_COUNT"

refuse "outputs write" "$CONFIGFS/stream1/outputs" 4
refuse "role write" "$CONFIGFS/stream1/role" host
refuse "session_index write" "$CONFIGFS/stream1/session_index" 5

# The item type holds a module reference while a group exists.
if rmmod hermes_kms 2>/dev/null; then
	printf 'FAIL: the module unloaded with a configfs group still present\n' >&2
	FAIL=1
	LOADED_BY_TEST=0
	exit 1
fi
printf 'ok: unload refused while a configfs group exists\n'

printf 0 > "$CONFIGFS/stream1/enabled"
sleep 0.5
check "card count after disable" 1 "$(hermes_cards)"
check "device_index after disable" -1 "$(cat "$CONFIGFS/stream1/device_index")"

# Re-enabling must work, and rmdir must take a live card with it.
printf 1 > "$CONFIGFS/stream1/enabled"
sleep 0.5
check "card count after re-enable" 2 "$(hermes_cards)"
rmdir "$CONFIGFS/stream1"
sleep 0.5
check "card count after rmdir" 1 "$(hermes_cards)"

# Several cards at once, to exercise id allocation and reuse.
for i in 1 2 3; do
	mkdir "$CONFIGFS/pool$i"
	printf 1 > "$CONFIGFS/pool$i/enabled"
done
sleep 0.5
check "card count with a pool of three" 4 "$(hermes_cards)"
for i in 1 2 3; do
	rmdir "$CONFIGFS/pool$i"
done
sleep 0.5
check "card count after removing the pool" 1 "$(hermes_cards)"

rmmod hermes_kms
LOADED_BY_TEST=0
[ -d /sys/module/hermes_kms ] && {
	printf 'FAIL: module still loaded after rmmod\n' >&2
	FAIL=1
}

if dmesg | grep -qiE 'BUG:|WARNING:|use-after-free|general protection'; then
	printf 'FAIL: kernel splat during the run\n' >&2
	dmesg | grep -iE -A15 'BUG:|WARNING:|use-after-free|general protection' >&2
	FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
	printf '\nPASS: runtime card creation through configfs\n'
else
	printf '\nFAIL: runtime card creation through configfs\n' >&2
fi
exit "$FAIL"
