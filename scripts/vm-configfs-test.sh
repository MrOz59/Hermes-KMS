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
ACCESS_RULE=/run/udev/rules.d/92-hermes-kms-access.rules
FAIL=0
LOADED_BY_TEST=0
RULE_INSTALLED=0

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
	if [ "$RULE_INSTALLED" -eq 1 ]; then
		rm -f -- "$ACCESS_RULE"
		udevadm control --reload-rules 2>/dev/null || true
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

install -Dm0644 "$REPO/udev/92-hermes-kms-access.rules" "$ACCESS_RULE"
RULE_INSTALLED=1
udevadm control --reload-rules

if insmod "$KO" min_width=64 min_height=64 max_width=64 max_height=64 \
	initial_width=64 initial_height=64 initial_refresh_hz=1 \
	max_refresh_hz=1 2>/dev/null; then
	printf 'FAIL: static card without an EDID-encodable timing loaded\n' >&2
	FAIL=1
	rmmod hermes_kms
else
	printf 'ok: static card without an EDID-encodable timing rejected\n'
fi

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
printf 1024 > "$CONFIGFS/stream1/serial_base"
printf 2560 > "$CONFIGFS/stream1/max_width"
printf 1440 > "$CONFIGFS/stream1/max_height"
printf VRT > "$CONFIGFS/stream1/manufacturer"
printf 'Virtual KMS' > "$CONFIGFS/stream1/monitor_name"
printf 'PROJECT-' > "$CONFIGFS/stream1/output_prefix"
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
check "stable configured output name" PROJECT-1024 \
	"$(printf '%s\n' "$IDENTITY" | value output)"
check "per-card mode envelope" 2560x1440 \
	"$("$CTL" --device "/dev/dri/$CARD" caps | value max)"

# A second card must not be able to take the same seat/broker index.
mkdir "$CONFIGFS/conflicting-session"
printf session > "$CONFIGFS/conflicting-session/role"
printf 3 > "$CONFIGFS/conflicting-session/session_index"
if printf 1 > "$CONFIGFS/conflicting-session/enabled" 2>/dev/null; then
	printf 'FAIL: duplicate session_index was accepted\n' >&2
	FAIL=1
	printf 0 > "$CONFIGFS/conflicting-session/enabled"
else
	printf 'ok: duplicate session_index rejected\n'
fi
rmdir "$CONFIGFS/conflicting-session"

mkdir "$CONFIGFS/conflicting-serial"
printf 1024 > "$CONFIGFS/conflicting-serial/serial_base"
if printf 1 > "$CONFIGFS/conflicting-serial/enabled" 2>/dev/null; then
	printf 'FAIL: duplicate EDID serial was accepted\n' >&2
	FAIL=1
	printf 0 > "$CONFIGFS/conflicting-serial/enabled"
else
	printf 'ok: duplicate EDID serial rejected\n'
fi
rmdir "$CONFIGFS/conflicting-serial"
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
refuse "serial_base write" "$CONFIGFS/stream1/serial_base" 2048
refuse "max_width write" "$CONFIGFS/stream1/max_width" 3840

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
mkdir "$CONFIGFS/interloper"
printf 1 > "$CONFIGFS/interloper/enabled"
printf 1 > "$CONFIGFS/stream1/enabled"
sleep 0.5
check "card count after re-enable" 3 "$(hermes_cards)"
check "identity survives creation-order change" PROJECT-1024 \
	"$("$CTL" --device "/dev/dri/$(cat "$CONFIGFS/stream1/card")" identity | value output)"
rmdir "$CONFIGFS/stream1"
rmdir "$CONFIGFS/interloper"
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

# A card can name the uid that owns its render node. Without this the packaged
# pool grants one configured uid every Hermes render node, so its private cards
# are private from the desktop but not from each other.
mkdir "$CONFIGFS/tenant-a"
mkdir "$CONFIGFS/tenant-b"
mkdir "$CONFIGFS/tenant-none"
mkdir "$CONFIGFS/tenant-ghost"
# bin and daemon, which exist on every distribution, stand in for two real
# consumer accounts; udev only applies an OWNER that resolves.
printf 1 > "$CONFIGFS/tenant-a/access_uid"
printf 2 > "$CONFIGFS/tenant-b/access_uid"
printf 4001 > "$CONFIGFS/tenant-ghost/access_uid"
check "access_uid default" 0 "$(cat "$CONFIGFS/tenant-none/access_uid")"
for name in tenant-a tenant-b tenant-none tenant-ghost; do
	printf 1 > "$CONFIGFS/$name/enabled"
done
sleep 0.5
udevadm trigger --subsystem-match=drm --action=change
udevadm settle
sleep 0.3

check "tenant-a render node owner" 1 \
	"$(stat -c %u "/dev/dri/$(cat "$CONFIGFS/tenant-a/render_node")")"
check "tenant-b render node owner" 2 \
	"$(stat -c %u "/dev/dri/$(cat "$CONFIGFS/tenant-b/render_node")")"
check "tenant-a render node mode" 600 \
	"$(stat -c %a "/dev/dri/$(cat "$CONFIGFS/tenant-a/render_node")")"
# An access_uid that resolves to no account must deny rather than fall back to
# whatever broader grant the installation already has.
check "unresolvable access_uid denies" 0 \
	"$(stat -c %u "/dev/dri/$(cat "$CONFIGFS/tenant-ghost/render_node")")"
# A card that names no uid must fall through to whatever policy already exists,
# not be assigned to root by this rule.
NONE_OWNER="$(stat -c %u "/dev/dri/$(cat "$CONFIGFS/tenant-none/render_node")")"
if [ "$NONE_OWNER" = 4001 ] || [ "$NONE_OWNER" = 4002 ]; then
	printf 'FAIL: a card naming no uid picked up another card owner (%s)\n' \
		"$NONE_OWNER" >&2
	FAIL=1
else
	printf 'ok: a card naming no uid was left alone (owner %s)\n' \
		"$NONE_OWNER"
fi
# The primary node keeps its role-based policy regardless.
check "tenant-a card node owner" 0 \
	"$(stat -c %u "/dev/dri/$(cat "$CONFIGFS/tenant-a/card")")"

refuse "access_uid write" "$CONFIGFS/tenant-a/access_uid" 4003

for name in tenant-a tenant-b tenant-none tenant-ghost; do
	rmdir "$CONFIGFS/$name"
done
sleep 0.5
check "card count after removing the tenants" 1 "$(hermes_cards)"

rmmod hermes_kms
LOADED_BY_TEST=0
[ -d /sys/module/hermes_kms ] && {
	printf 'FAIL: module still loaded after rmmod\n' >&2
	FAIL=1
}

# Projects using only runtime cards need not expose an unused static card.
insmod "$KO" devices=0 initial_enabled=0
LOADED_BY_TEST=1
check "configfs-only initial card count" 0 "$(hermes_cards)"
mkdir "$CONFIGFS/invalid-edid"
for dimension in width height; do
	printf 64 > "$CONFIGFS/invalid-edid/min_$dimension"
	printf 64 > "$CONFIGFS/invalid-edid/max_$dimension"
	printf 64 > "$CONFIGFS/invalid-edid/initial_$dimension"
done
printf 1 > "$CONFIGFS/invalid-edid/max_refresh_hz"
printf 1 > "$CONFIGFS/invalid-edid/initial_refresh_hz"
if printf 1 > "$CONFIGFS/invalid-edid/enabled" 2>/dev/null; then
	printf 'FAIL: a profile without an EDID-encodable timing was accepted\n' >&2
	FAIL=1
	printf 0 > "$CONFIGFS/invalid-edid/enabled"
else
	printf 'ok: profile without an EDID-encodable timing rejected\n'
fi
rmdir "$CONFIGFS/invalid-edid"
mkdir "$CONFIGFS/runtime-only"
default_serial="$(cat "$CONFIGFS/runtime-only/serial_base")"
if printf 12 > "$CONFIGFS/runtime-only/color_depth" 2>/dev/null; then
	printf 'FAIL: color depth without a scanout format was accepted\n' >&2
	FAIL=1
else
	printf 'ok: unsupported color depth rejected\n'
fi
printf 10 > "$CONFIGFS/runtime-only/color_depth"
printf 1280 > "$CONFIGFS/runtime-only/max_width"
printf 720 > "$CONFIGFS/runtime-only/max_height"
printf 1280 > "$CONFIGFS/runtime-only/initial_width"
printf 720 > "$CONFIGFS/runtime-only/initial_height"
printf 1 > "$CONFIGFS/runtime-only/hdr_enable"
printf 1 > "$CONFIGFS/runtime-only/initial_enabled"
printf 600 > "$CONFIGFS/runtime-only/physical_width_mm"
printf 340 > "$CONFIGFS/runtime-only/physical_height_mm"
printf 1 > "$CONFIGFS/runtime-only/enabled"
check "configfs-only live card count" 1 "$(hermes_cards)"
check "generic default output name" "VIRTUAL-$default_serial" \
	"$("$CTL" --device "/dev/dri/$(cat "$CONFIGFS/runtime-only/card")" identity | value output)"
runtime_card="$(cat "$CONFIGFS/runtime-only/card")"
runtime_modes="$(modetest -M hermes-kms -c)"
if printf '%s\n' "$runtime_modes" | grep -q '1920x1080'; then
	printf 'FAIL: a 720p card advertised the unsupported 1080p mode\n' >&2
	FAIL=1
else
	printf 'ok: 720p card mode list excludes 1080p\n'
fi
runtime_connector="$(find /sys/class/drm -maxdepth 1 -name "$runtime_card-Virtual-*" | head -n 1)"
[ -n "$runtime_connector" ] || { printf 'FAIL: runtime connector missing\n' >&2; FAIL=1; }
if [ -n "$runtime_connector" ]; then
	check "per-card HDR EDID size" 256 "$(wc -c < "$runtime_connector/edid")"
	if edid-decode --check "$runtime_connector/edid" >/dev/null 2>&1; then
		printf 'ok: per-card branded HDR EDID is conformant\n'
	else
		printf 'FAIL: per-card branded HDR EDID is non-conformant\n' >&2
		FAIL=1
	fi
	if edid-decode "$runtime_connector/edid" |
		grep -q 'DTD 1:  1280x720'; then
		printf 'ok: per-card EDID prefers the configured 720p mode\n'
	else
		printf 'FAIL: per-card EDID did not prefer 720p\n' >&2
		FAIL=1
	fi
fi
rmdir "$CONFIGFS/runtime-only"
rmmod hermes_kms
LOADED_BY_TEST=0

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
