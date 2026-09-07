#!/usr/bin/env bash
# Hermes-KMS HDR advertisement smoke test.
#
# Intended for a disposable virtme-ng guest:
#
#   virtme-ng --run --cwd "$PWD" --exec \
#     "bash scripts/vm-hdr-test.sh"
#
# Needs no GPU and no compositor. Everything hdr_enable does is advertisement --
# EDID bytes and connector properties -- and all of it is observable through
# sysfs and modetest on a headless guest. What cannot be checked here is whether
# a compositor then enables HDR, or what a consumer receives; that needs
# hardware and is out of scope.
#
# The three things this pins down:
#
#   1. The module loads with hdr_enable=1 at all. The property-attach helper it
#      calls changed return type between 7.1 and 7.2, so this is where a build
#      that compiled against the wrong assumption would surface.
#   2. The published EDID is 128 bytes with hdr_enable=0 and 256 with
#      hdr_enable=1. The driver's buffer is always 256, so publishing the
#      buffer's capacity instead of the EDID's own length is invisible on the
#      enabled path and only shows up here, on the disabled one.
#   3. Colorspace exposes both BT2020_RGB and Default, and HDR_OUTPUT_METADATA
#      is present -- and both are absent with hdr_enable=0. Default is checked
#      because the driver deliberately does not request it: the DRM helper ORs
#      it in, and this is what proves that still holds.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
TEST_TMP="$(mktemp -d /tmp/hermes-hdr.XXXXXX)"
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
trap 'exit 1' HUP INT TERM

check()
{
	local what="$1" expected="$2" actual="$3"

	if [ "$actual" != "$expected" ]; then
		printf 'FAIL: %s expected %s, got %s\n' \
			"$what" "$expected" "${actual:-<empty>}" >&2
		FAIL=1
	else
		printf 'ok: %s = %s\n' "$what" "$actual"
	fi
}

# Assert a substring is present in (or absent from) a file, naming what was
# being looked for rather than dumping the whole capture on failure.
contains()
{
	local what="$1" needle="$2" file="$3"

	if grep -qF -- "$needle" "$file"; then
		printf 'ok: %s\n' "$what"
	else
		printf 'FAIL: %s (no "%s" in %s)\n' "$what" "$needle" "$file" >&2
		FAIL=1
	fi
}

lacks()
{
	local what="$1" needle="$2" file="$3"

	if grep -qF -- "$needle" "$file"; then
		printf 'FAIL: %s ("%s" present in %s, expected absent)\n' \
			"$what" "$needle" "$file" >&2
		FAIL=1
	else
		printf 'ok: %s\n' "$what"
	fi
}

# The connector's EDID lives under the Hermes card's own sysfs node. Find the
# card by which driver it is bound to rather than by a fixed cardN index, which
# depends on what else the guest probed first. The connector is a
# DRM_MODE_CONNECTOR_VIRTUAL, so DRM names its directory "cardN-Virtual-M" --
# the driver's own "HERMES-n" output name is a separate thing that does not
# appear here.
hermes_connector_dir()
{
	local card dir

	for card in /sys/class/drm/card[0-9]*; do
		# Skip the connector directories this glob also matches; only the
		# bare "cardN" entries have a bound driver to inspect.
		case "${card##*/}" in
		*-*) continue ;;
		esac
		[ -e "$card/device/driver" ] || continue
		[ "$(basename "$(readlink -f "$card/device/driver")")" = hermes-kms ] ||
			continue

		for dir in "$card"-*; do
			[ -e "$dir/edid" ] || continue
			printf '%s\n' "$dir"
			return 0
		done
	done
	return 1
}

[ "$(id -u)" -eq 0 ] || {
	printf 'run as root (virtme-ng --exec runs as root by default)\n' >&2
	exit 1
}
[ -f "$KO" ] || { printf 'module not built: %s\n' "$KO" >&2; exit 1; }
command -v modetest >/dev/null || {
	printf 'modetest is required\n' >&2
	exit 1
}
[ ! -d /sys/module/hermes_kms ] || {
	printf 'hermes_kms is already loaded; use a disposable VM or unload it first\n' >&2
	exit 1
}

printf '=== kernel %s ===\n' "$(uname -r)"

# ---------------------------------------------------------------------------
# hdr_enable=0: the default, and the path that must stay byte-for-byte as it was
# ---------------------------------------------------------------------------
printf '\n--- hdr_enable=0 ---\n'
insmod "$KO" initial_enabled=1 hotplug_events=0 outputs=1
LOADED_BY_TEST=1
sleep 0.5

CONN="$(hermes_connector_dir)" || {
	printf 'FAIL: no Hermes connector found in /sys/class/drm\n' >&2
	exit 1
}
printf 'connector: %s\n' "$CONN"

check "hdr_enable module parameter" "N" \
	"$(cat /sys/module/hermes_kms/parameters/hdr_enable)"

# The connector's EDID property is only filled in when get_modes() runs, and
# that is driven by a connector probe rather than by module load. So probe
# first: reading sysfs straight after insmod returns an empty blob and says
# nothing about what the driver would publish.
timeout -k 1s 20s modetest -M hermes-kms -c > "$TEST_TMP/sdr-props.log" 2>&1 || true

# The published EDID length is the whole point of this half of the test.
cp "$CONN/edid" "$TEST_TMP/sdr.bin"
check "published EDID size" "128" "$(stat -c%s "$TEST_TMP/sdr.bin")"
check "extension count byte" "0" \
	"$(od -An -tu1 -j126 -N1 "$TEST_TMP/sdr.bin" | tr -d ' ')"

lacks "no Colorspace property when disabled" "Colorspace" "$TEST_TMP/sdr-props.log"
lacks "no HDR_OUTPUT_METADATA when disabled" "HDR_OUTPUT_METADATA" \
	"$TEST_TMP/sdr-props.log"

rmmod hermes_kms
LOADED_BY_TEST=0
sleep 0.3

# ---------------------------------------------------------------------------
# hdr_enable=1: the advertisement itself
# ---------------------------------------------------------------------------
printf '\n--- hdr_enable=1 ---\n'
insmod "$KO" initial_enabled=1 hotplug_events=0 outputs=1 hdr_enable=1
LOADED_BY_TEST=1
sleep 0.5

CONN="$(hermes_connector_dir)" || {
	printf 'FAIL: no Hermes connector found with hdr_enable=1\n' >&2
	exit 1
}

check "hdr_enable module parameter" "Y" \
	"$(cat /sys/module/hermes_kms/parameters/hdr_enable)"

# Same as above: probe before reading, so the blob exists to be measured.
timeout -k 1s 20s modetest -M hermes-kms -c > "$TEST_TMP/hdr-props.log" 2>&1 || true

cp "$CONN/edid" "$TEST_TMP/hdr.bin"
check "published EDID size" "256" "$(stat -c%s "$TEST_TMP/hdr.bin")"
check "extension count byte" "1" \
	"$(od -An -tu1 -j126 -N1 "$TEST_TMP/hdr.bin" | tr -d ' ')"
check "CTA extension tag" "2" \
	"$(od -An -tu1 -j128 -N1 "$TEST_TMP/hdr.bin" | tr -d ' ')"

# The base block must be unchanged apart from the extension count and checksum:
# enabling HDR appends, it does not rewrite the identity a compositor keys on
# when it persists a layout. Both phases run at the same colour depth, so any
# difference in the first 126 bytes is the append overreaching. (color_depth is
# checked separately below, precisely because it does move a base-block byte.)
check "base block bytes 0..125 unchanged by the append" "same" \
	"$(cmp -s -n 126 "$TEST_TMP/sdr.bin" "$TEST_TMP/hdr.bin" && \
		echo same || echo differs)"

# Let a real parser confirm the blocks rather than trusting the byte offsets
# above. Optional: a minimal guest may not carry edid-decode.
if command -v edid-decode >/dev/null 2>&1; then
	edid-decode < "$TEST_TMP/hdr.bin" > "$TEST_TMP/decode.log" 2>&1 || true
	contains "edid-decode sees the HDR Static Metadata block" \
		"HDR Static Metadata Data Block" "$TEST_TMP/decode.log"
	contains "edid-decode sees the Colorimetry block" \
		"Colorimetry Data Block" "$TEST_TMP/decode.log"
	contains "edid-decode sees PQ" "SMPTE ST2084" "$TEST_TMP/decode.log"
	contains "edid-decode sees BT2020 RGB" "BT2020RGB" "$TEST_TMP/decode.log"
	lacks "no checksum error" "Checksum Error" "$TEST_TMP/decode.log"
else
	printf 'skip: edid-decode not present in the guest\n'
fi

contains "HDR_OUTPUT_METADATA on the connector" "HDR_OUTPUT_METADATA" \
	"$TEST_TMP/hdr-props.log"
contains "Colorspace on the connector" "Colorspace" "$TEST_TMP/hdr-props.log"
contains "Colorspace offers BT2020_RGB" "BT2020_RGB" "$TEST_TMP/hdr-props.log"
# The driver asks for BT2020_RGB only; DRM is expected to add Default itself.
contains "Colorspace still offers Default" "Default" "$TEST_TMP/hdr-props.log"

rmmod hermes_kms
LOADED_BY_TEST=0
sleep 0.3
check "module unloaded cleanly" "gone" \
	"$([ -d /sys/module/hermes_kms ] && echo present || echo gone)"

# ---------------------------------------------------------------------------
# hdr_enable=1 with color_depth=10: the pairing the documentation discusses.
# Nothing here claims HDR works end to end -- only that the two load together
# and that ten-bit reaches the EDID, since the docs tell users to try them
# together and a load-time interaction between them would strand that advice.
# ---------------------------------------------------------------------------
printf '\n--- hdr_enable=1 color_depth=10 ---\n'
insmod "$KO" initial_enabled=1 hotplug_events=0 outputs=1 hdr_enable=1 color_depth=10
LOADED_BY_TEST=1
sleep 0.5

CONN="$(hermes_connector_dir)" || {
	printf 'FAIL: no Hermes connector found with hdr_enable=1 color_depth=10\n' >&2
	exit 1
}
timeout -k 1s 20s modetest -M hermes-kms -c > "$TEST_TMP/ten-bit.log" 2>&1 || true
cp "$CONN/edid" "$TEST_TMP/ten-bit.bin"

check "published EDID size" "256" "$(stat -c%s "$TEST_TMP/ten-bit.bin")"
# Video input byte: 0x80 sets digital, bits 6:4 carry the depth field, and the
# EDID depth code for ten bits per primary is 3 -- so 0x80 | (3 << 4) = 0xb0.
check "EDID states ten bits per primary" "b0" \
	"$(od -An -tx1 -j20 -N1 "$TEST_TMP/ten-bit.bin" | tr -d ' ')"
contains "HDR_OUTPUT_METADATA still attached at ten bits" "HDR_OUTPUT_METADATA" \
	"$TEST_TMP/ten-bit.log"
contains "Colorspace still attached at ten bits" "BT2020_RGB" "$TEST_TMP/ten-bit.log"

rmmod hermes_kms
LOADED_BY_TEST=0
sleep 0.3
check "module unloaded cleanly" "gone" \
	"$([ -d /sys/module/hermes_kms ] && echo present || echo gone)"

# ---------------------------------------------------------------------------
# Nothing in the kernel log should have complained.
# ---------------------------------------------------------------------------
printf '\n--- kernel log ---\n'
dmesg | grep -i hermes | tail -20 || true
if dmesg | grep -iE "hermes.*(BUG|WARN|Oops|refused an invalid base)" > \
	"$TEST_TMP/splat.log" 2>&1; then
	printf 'FAIL: kernel complaints about hermes-kms:\n' >&2
	cat "$TEST_TMP/splat.log" >&2
	FAIL=1
else
	printf 'ok: no hermes-kms warnings or splats in dmesg\n'
fi

printf '\n'
if [ "$FAIL" -ne 0 ]; then
	printf 'HDR advertisement test: FAIL\n' >&2
	exit 1
fi
printf 'HDR advertisement test: PASS\n'
