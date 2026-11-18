#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Exercises the KWin render bridge end to end without a GPU or a compositor:
# fake libgbm/libdrm/libkwin, a scenario driver posing as kwin_wayland, a
# fake sysfs tree, and /dev character devices standing in for DRM nodes:
#
#   /dev/null    1:3  Hermes-KMS card1 (primary)
#   /dev/full    1:7  Hermes-KMS renderD129
#   /dev/zero    1:5  NVIDIA renderD128, 0000:01:00.0
#   /dev/random  1:8  amdgpu card0
#   /dev/urandom 1:9  amdgpu renderD130, 0000:03:00.0
#
# Usage: tests/kwin-bridge/run.sh [CC] ; BRIDGE_CFLAGS must locate xf86drm.h.
set -eu

CC=${1:-${CC:-cc}}
BRIDGE_CFLAGS=${BRIDGE_CFLAGS:-$(pkg-config --cflags libdrm 2>/dev/null || true)}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/hermes-kms-kwin-bridge.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
trap 'exit 1' HUP INT TERM

for node in null zero full random urandom; do
	[ -c "/dev/$node" ] || { echo "kwin-bridge: /dev/$node missing; skipped"; exit 0; }
done
case "$(stat -c '%t:%T' /dev/null /dev/zero /dev/full /dev/random /dev/urandom | tr '\n' ' ')" in
	"1:3 1:5 1:7 1:8 1:9 ") ;;
	*) echo "kwin-bridge: unexpected /dev numbering; skipped"; exit 0 ;;
esac

CFLAGS="-O2 -g -Wall -Wextra -Werror -std=gnu11 -fPIC"
mkdir -p "$T/lib" "$T/kwin68" "$T/kwin69" "$T/preload"

"$CC" $CFLAGS -shared -DFAKE_GBM "$HERE/fake_libs.c" \
	-Wl,-soname,libgbm.so.1 -o "$T/lib/libgbm.so.1"
ln -s libgbm.so.1 "$T/lib/libgbm.so"
"$CC" $CFLAGS $BRIDGE_CFLAGS -shared -DFAKE_DRM "$HERE/fake_libs.c" \
	-Wl,-soname,libdrm.so.2 -o "$T/lib/libdrm.so.2"
ln -s libdrm.so.2 "$T/lib/libdrm.so"
for release in 6.8.4:kwin68 6.9.0:kwin69; do
	version=${release%%:*}
	dir=$T/${release##*:}
	"$CC" $CFLAGS -shared -DFAKE_KWIN "$HERE/fake_libs.c" \
		-Wl,-soname,libkwin.so.6 -L"$T/lib" -lgbm \
		-o "$dir/libkwin.so.$version"
	ln -s "libkwin.so.$version" "$dir/libkwin.so.6"
	ln -s "libkwin.so.$version" "$dir/libkwin.so"
done
"$CC" $CFLAGS "$HERE/fake_kwin_wayland.c" -L"$T/kwin68" -L"$T/lib" \
	-Wl,--no-as-needed -lkwin -lgbm -ldrm -o "$T/kwin_wayland"
"$CC" $CFLAGS $BRIDGE_CFLAGS -DHKB_TESTING -shared -fvisibility=hidden \
	"$ROOT/tools/hermes-kms-kwin-bridge/hermes_kms_kwin_bridge.c" \
	-o "$T/preload/libhermes-kms-kwin-bridge.so" -ldl -pthread
printf 'int hkb_noop;\n' > "$T/noop.c"
"$CC" $CFLAGS -shared "$T/noop.c" -o "$T/preload/libhkb-noop.so"

# Fake sysfs
S=$T/sys
mkdir -p "$S/dev/char" "$S/bus/platform/drivers/hermes-kms" \
	"$S/bus/pci/drivers/nvidia" "$S/bus/pci/drivers/amdgpu"
P=$S/devices/platform/hermes-kms.0
N=$S/devices/pci0000:00/0000:01:00.0
A=$S/devices/pci0000:00/0000:03:00.0
mkdir -p "$P/drm/card1" "$P/drm/renderD129" "$N/drm/renderD128" \
	"$A/drm/card0" "$A/drm/renderD130"
ln -s "$S/bus/platform/drivers/hermes-kms" "$P/driver"
ln -s "$S/bus/pci/drivers/nvidia" "$N/driver"
ln -s "$S/bus/pci/drivers/amdgpu" "$A/driver"
for node in "$P/drm/card1:1:3" "$P/drm/renderD129:1:7" \
	"$N/drm/renderD128:1:5" "$A/drm/card0:1:8" "$A/drm/renderD130:1:9"; do
	path=${node%:*:*}
	dev=${node#"$path":}
	ln -s "$(dirname "$(dirname "$path")")" "$path/device"
	echo "$dev" > "$path/dev"
	ln -s "$path" "$S/dev/char/$dev"
done

BRIDGE=$T/preload/libhermes-kms-kwin-bridge.so
NOOP=$T/preload/libhkb-noop.so
EXE=$(readlink -f "$T/kwin_wayland")
GPUS="10de@0000:01:00.0=/dev/zero;1002@0000:03:00.0=/dev/urandom"
failures=0

# run NAME [VAR=value...] -- steps...
run() {
	name=$1
	shift
	set -- env -i PATH="$PATH" LD_LIBRARY_PATH="$T/kwin68:$T/lib" \
		HKB_TEST_SYSFS_ROOT="$S" HKB_TEST_KWIN_EXE="$EXE" \
		HKB_FAKE_GPUS="$GPUS" HERMES_KMS_KWIN_BRIDGE_ACTIVE=1 \
		LD_PRELOAD="$BRIDGE" "$@"
	out=$("$@" 2>&1) || { echo "FAIL $name: exit $?"; echo "$out"; failures=$((failures + 1)); out=; }
	current=$name
}

expect() {
	if ! printf '%s\n' "$out" | grep -Fqx -- "$1"; then
		echo "FAIL $current: missing line: $1"
		printf '%s\n' "$out" | sed 's/^/    /'
		failures=$((failures + 1))
	fi
}

expect_log() {
	if ! printf '%s\n' "$out" | grep -Fq -- "$1"; then
		echo "FAIL $current: missing log: $1"
		printf '%s\n' "$out" | sed 's/^/    /'
		failures=$((failures + 1))
	fi
}

expect_no_log() {
	if printf '%s\n' "$out" | grep -Fq -- "$1"; then
		echo "FAIL $current: unexpected: $1"
		printf '%s\n' "$out" | sed 's/^/    /'
		failures=$((failures + 1))
	fi
}

MAP="map:1:5=nvidia map:1:3=software map:1:7=software map:1:8=amd map:1:9=amd"

# shellcheck disable=SC2086
run routed LD_PRELOAD="$BRIDGE $NOOP" "$T/kwin_wayland" env $MAP \
	open:/dev/null compat:1:3 compatdev:1:3 compat:1:7 compat:1:8 \
	dumb:0 direct:0 scanout:0 \
	open:/dev/null open:/dev/full open:/dev/random \
	close:0 compat:1:3 compatdev:1:3
expect "env LD_PRELOAD=$NOOP marker=(unset)"
expect "open 0 rdev=1:5"
expect "compat 1:3 -> nvidia"
expect "compatdev 1:3 -> nvidia"
expect "compat 1:7 -> nvidia"
expect "compat 1:8 -> amd"
expect "dumb 0 rdev=1:3"
expect "direct 0 rdev=1:5"
expect "scanout 0 ok=1 flags=0x5"
expect "open 1 rdev=1:3"
expect "open 2 rdev=1:7"
expect "open 3 rdev=1:8"
expect "compat 1:3 -> software"
expect "compatdev 1:3 -> software"
expect_log "active in kwin_wayland (libkwin.so.6.8.4)"
expect_log "Hermes-KMS card 1:3 allocates on /dev/zero (0000:01:00.0)"
expect_log "KWin renders Hermes-KMS card 1:3 on 0000:01:00.0"
expect_log "second GBM device for Hermes-KMS card 1:3 left native"
expect_log "first GPU buffer for card 1:3: 1920x1080 format 0x34325258, 1 candidate modifier(s), flags 0x5"
expect_no_log "before asking for its render device"

# shellcheck disable=SC2086
run dumb-first "$T/kwin_wayland" $MAP open:/dev/null dumb:0
expect "dumb 0 rdev=1:3"
expect_log "before asking for its render device"

run scanout-refused HKB_FAKE_GBM_NO_SCANOUT=1 "$T/kwin_wayland" \
	open:/dev/null scanout:0 open:/dev/random scanout:1
expect "scanout 0 ok=1 flags=0x4"
expect "scanout 1 ok=0 flags=0x5"
expect_log "refused GBM_BO_USE_SCANOUT on its render node; allocated without it"

# shellcheck disable=SC2086
run unreviewed-kwin LD_LIBRARY_PATH="$T/kwin69:$T/lib" "$T/kwin_wayland" \
	$MAP open:/dev/null compat:1:3
expect "open 0 rdev=1:3"
expect "compat 1:3 -> software"
expect_log "KWin 6.9 has not been reviewed for this bridge"

run forced-kwin LD_LIBRARY_PATH="$T/kwin69:$T/lib" \
	HERMES_KMS_KWIN_BRIDGE=force "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:5"
expect_log "KWin 6.9 is unreviewed; continuing because HERMES_KMS_KWIN_BRIDGE=force"

run off HERMES_KMS_KWIN_BRIDGE=off "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:3"
expect_log "disabled by HERMES_KMS_KWIN_BRIDGE"

run invalid-mode HERMES_KMS_KWIN_BRIDGE=maybe "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:3"
expect_log "HERMES_KMS_KWIN_BRIDGE=maybe is not auto, off or force"

run not-kwin HKB_TEST_KWIN_EXE=/usr/bin/kwin_wayland "$T/kwin_wayland" \
	env open:/dev/null
expect "env LD_PRELOAD=$BRIDGE marker=1"
expect "open 0 rdev=1:3"
expect_no_log "hermes-kms-kwin-bridge:"

run no-marker HERMES_KMS_KWIN_BRIDGE_ACTIVE= "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:3"
expect_no_log "hermes-kms-kwin-bridge:"

# shellcheck disable=SC2086
run explicit-pci HERMES_KMS_KWIN_RENDER_PCI=0000:03:00.0 "$T/kwin_wayland" \
	$MAP open:/dev/null compat:1:3
expect "open 0 rdev=1:9"
expect "compat 1:3 -> amd"

run invalid-pci HERMES_KMS_KWIN_RENDER_PCI=0000:3:0 "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:3"
expect_log "HERMES_KMS_KWIN_RENDER_PCI=0000:3:0 is not a PCI address"

run missing-pci HERMES_KMS_KWIN_RENDER_PCI=0000:09:00.0 "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:3"
expect_log "no render node at HERMES_KMS_KWIN_RENDER_PCI=0000:09:00.0"

run no-nvidia HKB_FAKE_GPUS="1002@0000:03:00.0=/dev/urandom" "$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:3"
expect_log "no NVIDIA render node found"

run lowest-nvidia HKB_FAKE_GPUS="10de@0000:05:00.0=/dev/urandom;10de@0000:01:00.0=/dev/zero" \
	"$T/kwin_wayland" open:/dev/null
expect "open 0 rdev=1:5"

# The installed library must do nothing when executed directly.
case "$(uname -m)" in
	x86_64|aarch64)
		"$CC" -O2 -fPIC -fvisibility=hidden $BRIDGE_CFLAGS -shared \
			-Wl,-e,hkb_exec_entry \
			"$ROOT/tools/hermes-kms-kwin-bridge/hermes_kms_kwin_bridge.c" \
			-o "$T/exec-test.so" -ldl -pthread
		chmod +x "$T/exec-test.so"
		status=0
		"$T/exec-test.so" >/dev/null 2>&1 || status=$?
		current=exec-entry
		[ "$status" = 1 ] || { echo "FAIL exec-entry: exit $status"; failures=$((failures + 1)); }
		;;
esac

if [ "$failures" -ne 0 ]; then
	echo "kwin-bridge: $failures failure(s)"
	exit 1
fi
echo "kwin-bridge: ok"
