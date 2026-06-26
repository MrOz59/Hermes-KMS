#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CTL="$REPO_ROOT/tools/hermes-kmsctl/hermes-kmsctl"
IMPORT_CHECK="$REPO_ROOT/tools/hermes-kms-import-check/hermes-kms-import-check"
KO="$REPO_ROOT/kernel/hermes-kms/hermes_kms.ko"
EXPECTED_UAPI_VERSION="$(awk '/#define HERMES_KMS_UAPI_VERSION/ { print $3; exit }' "$REPO_ROOT/include/uapi/drm/hermes_kms_drm.h")"

MODE="1920x1080@60"
DEVICE=""
CONTROL_DEVICE=""
SKIP_BUILD=0
SKIP_LOAD=0
NO_RELOAD=0
KEEP_LOADED=0
AUTO_MODETEST=1
PRODUCER_CMD=""
CHECK_VAAPI_IMPORT=0
VA_DEVICE=""
LOADED_BY_SCRIPT=0
OUTPUT_READY_WITHOUT_OWNER=0
OWNER_PID=""
PRODUCER_PID=""
WORK_DIR="$REPO_ROOT/build/hermes-kms-zero-copy-$UID"

usage()
{
	cat <<EOF
Usage: $0 [options]

Build/load Hermes-KMS and verify the zero-copy-facing DMA-BUF path.

Options:
  --mode WIDTHxHEIGHT@HZ     Mode to request before testing (default: $MODE)
  --device /dev/dri/cardN    Use a specific Hermes-KMS DRM device
  --no-build                 Do not run make before testing
  --no-load                  Do not insmod the module; use an already-loaded one
  --no-reload                Keep an already-loaded hotplug_events=1 module instead
                             of reloading it into isolated (modetest) mode
  --keep-loaded              Do not unload the module if this script loaded it
  --no-modetest              Do not try to create a scanout framebuffer with modetest
  --producer-cmd CMD         Start CMD in the background before retrying frame acquire
  --check-vaapi-import       Try importing the exported DMA-BUF into VAAPI
  --va-device /dev/dri/renderDN
                             VAAPI DRM device for --check-vaapi-import
  -h, --help                 Show this help

Exit status:
  0  DMA-BUF frame export worked
  1  Driver/tool test failed
  2  Driver is reachable, but no active scanout framebuffer was available
EOF
}

log()
{
	printf '[hermes-kms-test] %s\n' "$*"
}

fail()
{
	printf '[hermes-kms-test] FAIL: %s\n' "$*" >&2
	exit 1
}

inconclusive()
{
	printf '[hermes-kms-test] INCONCLUSIVE: %s\n' "$*" >&2
	exit 2
}

cleanup()
{
	local rc=$?

	if [ -n "$PRODUCER_PID" ] && kill -0 "$PRODUCER_PID" 2>/dev/null; then
		kill "$PRODUCER_PID" 2>/dev/null || true
		wait "$PRODUCER_PID" 2>/dev/null || true
	fi

	if [ -n "$OWNER_PID" ] && kill -0 "$OWNER_PID" 2>/dev/null; then
		kill "$OWNER_PID" 2>/dev/null || true
		wait "$OWNER_PID" 2>/dev/null || true
	fi

	if [ "$LOADED_BY_SCRIPT" -eq 1 ] && [ "$KEEP_LOADED" -eq 0 ]; then
		log "unloading hermes_kms"
		run_as_root rmmod hermes_kms || true
	fi

	exit "$rc"
}

trap cleanup EXIT

while [ "$#" -gt 0 ]; do
	case "$1" in
		--mode)
			[ "$#" -ge 2 ] || fail "--mode needs a value"
			MODE="$2"
			shift 2
			;;
		--device)
			[ "$#" -ge 2 ] || fail "--device needs a value"
			DEVICE="$2"
			shift 2
			;;
		--no-build)
			SKIP_BUILD=1
			shift
			;;
		--no-load)
			SKIP_LOAD=1
			shift
			;;
		--no-reload)
			NO_RELOAD=1
			shift
			;;
		--keep-loaded)
			KEEP_LOADED=1
			shift
			;;
		--no-modetest)
			AUTO_MODETEST=0
			shift
			;;
		--producer-cmd)
			[ "$#" -ge 2 ] || fail "--producer-cmd needs a value"
			PRODUCER_CMD="$2"
			shift 2
			;;
		--check-vaapi-import)
			CHECK_VAAPI_IMPORT=1
			shift
			;;
		--va-device)
			[ "$#" -ge 2 ] || fail "--va-device needs a value"
			VA_DEVICE="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			fail "unknown option: $1"
			;;
	esac
done

if [[ ! "$MODE" =~ ^([0-9]+)x([0-9]+)@([0-9]+)$ ]]; then
	fail "invalid mode '$MODE', expected WIDTHxHEIGHT@HZ"
fi

MODE_WIDTH="${BASH_REMATCH[1]}"
MODE_HEIGHT="${BASH_REMATCH[2]}"
MODE_HZ="${BASH_REMATCH[3]}"

run_ctl()
{
	if [ -n "$CONTROL_DEVICE" ]; then
		"$CTL" --device "$CONTROL_DEVICE" "$@"
	elif [ -n "$DEVICE" ]; then
		"$CTL" --device "$DEVICE" "$@"
	else
		"$CTL" "$@"
	fi
}

discover_hermes_drm_node()
{
	local card
	local module_path
	local prefix="$1"

	for card in /sys/class/drm/"$prefix"*; do
		[ -e "$card" ] || continue
		[ -e "$card/device/driver/module" ] || continue

		module_path="$(readlink -f "$card/device/driver/module" 2>/dev/null || true)"
		if [ "$module_path" = "/sys/module/hermes_kms" ]; then
			printf '/dev/dri/%s\n' "$(basename "$card")"
			return 0
		fi
	done

	return 1
}

module_loaded()
{
	grep -qw '^hermes_kms' /proc/modules
}

# A Hermes-KMS card still tagged for the active seat will be opened by
# KWin/Xwayland, which takes DRM master and prevents isolated modetest from
# committing a scanout (and keeps the module busy on unload). The seat-ignore
# udev rule strips that tag; warn loudly if it has not been applied.
warn_if_seat_tagged()
{
	local node="$1"
	[ -n "$node" ] || return 0
	command -v udevadm >/dev/null 2>&1 || return 0

	# Check CURRENT_TAGS (the live runtime tags logind/seat managers act on),
	# not TAGS (the persisted udev db, which keeps seat/master-of-seat even
	# after our rule removes them at runtime via TAG-=). What makes
	# KWin/Xwayland grab the card is the live seat association, so a stale
	# TAGS entry is not a problem as long as CURRENT_TAGS is clean.
	local tags
	tags="$(udevadm info "$node" 2>/dev/null | grep -E '^E: CURRENT_TAGS=' || true)"
	case "$tags" in
		*:seat:*|*:master-of-seat:*)
			log "warning: $node is still tagged for the active seat (KWin/Xwayland will grab it)"
			log "warning: install the seat-ignore rule, then reload: sudo $REPO_ROOT/scripts/install-dev-seat-ignore.sh"
			;;
	esac
}

module_hotplug_events_enabled()
{
	local value

	[ -r /sys/module/hermes_kms/parameters/hotplug_events ] || return 1
	value="$(cat /sys/module/hermes_kms/parameters/hotplug_events)"
	[ "$value" = "Y" ] || [ "$value" = "1" ]
}

run_as_root()
{
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
		return
	fi

	if ! sudo -n true 2>/dev/null; then
		fail "root is required for '$*'. Run 'sudo -v' first, then rerun this script."
	fi

	sudo "$@"
}

get_value()
{
	local key="$1"
	awk -F= -v key="$key" '$1 == key { print $2; exit }'
}

require_cap()
{
	local caps="$1"
	local key="$2"
	local value

	value="$(printf '%s\n' "$caps" | get_value "$key")"
	if [ "$value" != "true" ]; then
		fail "required capability '$key' is '$value'"
	fi
	log "capability $key=true"
}

check_loaded_uapi_version()
{
	local version_output="$1"
	local loaded_uapi

	loaded_uapi="$(printf '%s\n' "$version_output" | get_value uapi_version)"
	if [ -z "$loaded_uapi" ]; then
		fail "could not read loaded Hermes-KMS UAPI version"
	fi

	if [ "$loaded_uapi" = "$EXPECTED_UAPI_VERSION" ]; then
		return 0
	fi

	log "loaded Hermes-KMS UAPI version is $loaded_uapi, but this repo expects $EXPECTED_UAPI_VERSION"
	log "an older hermes_kms module is still loaded; the build succeeded but the running kernel module was not replaced"

	if [ -n "$DEVICE" ] && command -v lsof >/dev/null 2>&1; then
		log "openers for $DEVICE:"
		lsof "$DEVICE" 2>&1 || true
	fi

	if [ -n "$CONTROL_DEVICE" ] && [ "$CONTROL_DEVICE" != "$DEVICE" ] &&
	   command -v lsof >/dev/null 2>&1; then
		log "openers for $CONTROL_DEVICE:"
		lsof "$CONTROL_DEVICE" 2>&1 || true
	fi

	fail "loaded hermes_kms module is stale; unload it first, then rerun. Try: sudo rmmod hermes_kms"
}

wait_for_device()
{
	local output=""
	local i

	for i in $(seq 1 30); do
		if output="$(run_ctl version 2>&1)"; then
			printf '%s\n' "$output"
			return 0
		fi
		sleep 0.2
	done

	printf '%s\n' "$output" >&2
	return 1
}

try_acquire_dmabuf()
{
	local output
	local rc

	set +e
	output="$(run_ctl frame --require-dmabuf --sync-file 2>&1)"
	rc=$?
	set -e

	printf '%s\n' "$output"
	return "$rc"
}

validate_frame_output()
{
	local output="$1"
	local metadata_valid
	local dmabuf_valid
	local sync_file_valid
	local copy_fallback_required
	local plane_count
	local fd_count

	metadata_valid="$(printf '%s\n' "$output" | get_value metadata_valid)"
	dmabuf_valid="$(printf '%s\n' "$output" | get_value dmabuf_valid)"
	sync_file_valid="$(printf '%s\n' "$output" | get_value sync_file_valid)"
	copy_fallback_required="$(printf '%s\n' "$output" | get_value copy_fallback_required)"
	plane_count="$(printf '%s\n' "$output" | get_value plane_count)"
	fd_count="$(printf '%s\n' "$output" | grep -Ec '^plane_[0-9]+_dma_buf_fd=[0-9]+$' || true)"

	[ "$metadata_valid" = "true" ] || fail "frame metadata was not valid"
	[ "$dmabuf_valid" = "true" ] || fail "frame DMA-BUF was not valid"
	[ "$sync_file_valid" = "true" ] || fail "frame sync_file was not valid"
	[ "$copy_fallback_required" = "false" ] || fail "driver reported copy fallback required"
	[ "${plane_count:-0}" -ge 1 ] || fail "frame reported no planes"
	[ "$fd_count" -ge 1 ] || fail "frame did not report any exported DMA-BUF fd"

	log "DMA-BUF frame export and sync_file export worked"
	log "frame: sequence=$(printf '%s\n' "$output" | get_value sequence), size=$(printf '%s\n' "$output" | get_value size), format=$(printf '%s\n' "$output" | get_value format), planes=$plane_count"
}

run_vaapi_import_check()
{
	local args

	[ "$CHECK_VAAPI_IMPORT" -eq 1 ] || return 0
	[ -x "$IMPORT_CHECK" ] || fail "$IMPORT_CHECK is missing or not executable"

	args=(--wait-ms 1000)
	if [ -n "$CONTROL_DEVICE" ]; then
		args+=(--device "$CONTROL_DEVICE")
	elif [ -n "$DEVICE" ]; then
		args+=(--device "$DEVICE")
	fi
	if [ -n "$VA_DEVICE" ]; then
		args+=(--va-device "$VA_DEVICE")
	fi

	log "checking VAAPI DMA-BUF import"
	"$IMPORT_CHECK" "${args[@]}"
}

start_output_owner()
{
	if [ "$OUTPUT_READY_WITHOUT_OWNER" -eq 1 ]; then
		log "using initial connected output; no control owner fd is needed"
		return 0
	fi

	mkdir -p "$WORK_DIR"

	log "starting Hermes-KMS output owner for $MODE"
	if [ -n "$CONTROL_DEVICE" ]; then
		"$CTL" --device "$CONTROL_DEVICE" hold "$MODE" >"$WORK_DIR/output-owner.log" 2>&1 &
	elif [ -n "$DEVICE" ]; then
		"$CTL" --device "$DEVICE" hold "$MODE" >"$WORK_DIR/output-owner.log" 2>&1 &
	else
		"$CTL" hold "$MODE" >"$WORK_DIR/output-owner.log" 2>&1 &
	fi
	OWNER_PID=$!
	sleep 1

	if ! kill -0 "$OWNER_PID" 2>/dev/null; then
		log "output owner exited; see $WORK_DIR/output-owner.log"
		return 1
	fi

	return 0
}

start_custom_producer()
{
	[ -n "$PRODUCER_CMD" ] || return 1

	log "starting producer command"
	mkdir -p "$WORK_DIR"
	bash -lc "$PRODUCER_CMD" >"$WORK_DIR/producer.log" 2>&1 &
	PRODUCER_PID=$!
	sleep 2

	if ! kill -0 "$PRODUCER_PID" 2>/dev/null; then
		log "producer command exited; see $WORK_DIR/producer.log"
	fi

	return 0
}

first_connected_connector()
{
	awk '
		/^Connectors:/ { in_connectors = 1; next }
		/^Encoders:|^CRTCs:|^Planes:|^Frame buffers:/ { in_connectors = 0 }
		in_connectors && $1 ~ /^[0-9]+$/ && $3 == "connected" { print $1; exit }
	'
}

first_crtc()
{
	awk '
		/^CRTCs:/ { in_crtcs = 1; next }
		/^Connectors:|^Encoders:|^Planes:|^Frame buffers:/ { if (in_crtcs) exit }
		in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
	'
}

first_primary_plane()
{
	awk '
		/^Planes:/ {
			in_planes = 1
			current_plane = ""
			in_type = 0
			next
		}
		/^Frame buffers:/ { in_planes = 0 }
		in_planes && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ && $NF ~ /^0x[0-9a-fA-F]+$/ {
			current_plane = $1
			in_type = 0
			next
		}
		in_planes && /^[[:space:]]+[0-9]+[[:space:]]+type:/ {
			in_type = 1
			next
		}
		in_planes && in_type && /^[[:space:]]+value:/ && $2 == "1" {
			print current_plane
			exit
		}
	'
}

first_non_desktop_value()
{
	awk '
		/non-desktop:/ { in_non_desktop = 1; next }
		in_non_desktop && /^[[:space:]]+value:/ { print $2; exit }
	'
}

start_modetest_producer()
{
	local info
	local connector
	local crtc
	local plane
	local non_desktop
	local modetest_mode
	local modetest_plane
	local modetest_device_args
	local modetest_rc

	[ "$AUTO_MODETEST" -eq 1 ] || return 1
	command -v modetest >/dev/null 2>&1 || return 1

	mkdir -p "$WORK_DIR"
	modetest_device_args=(-M hermes-kms)

	if ! info="$(modetest "${modetest_device_args[@]}" 2>"$WORK_DIR/modetest-probe.err")"; then
		log "modetest probe failed; see $WORK_DIR/modetest-probe.err"
		return 1
	fi

	connector="$(printf '%s\n' "$info" | first_connected_connector)"
	crtc="$(printf '%s\n' "$info" | first_crtc)"
	plane="$(printf '%s\n' "$info" | first_primary_plane)"
	non_desktop="$(printf '%s\n' "$info" | first_non_desktop_value)"

	if [ "$non_desktop" = "0" ]; then
		log "warning: Hermes-KMS connector reports non-desktop=0; reload the module if this is not the latest build"
	fi

	if [ -z "$connector" ] || [ -z "$crtc" ] || [ -z "$plane" ]; then
		log "modetest could not find a connected connector, CRTC, and primary plane"
		printf '%s\n' "$info" >"$WORK_DIR/modetest-probe.txt"
		return 1
	fi

	# Match the mode by name only (WIDTHxHEIGHT), not "WIDTHxHEIGHT-REFRESH".
	# libdrm's modetest formats the lookup name as "<name>-<rate>.00Hz" and
	# compares it against the kernel mode name, which is just "WIDTHxHEIGHT"
	# (drm_mode_set_name does not embed the refresh). Passing the -REFRESH
	# suffix therefore never matches. The Hermes connector advertises the
	# requested refresh as the preferred mode for this resolution, so a
	# name-only match selects it.
	modetest_mode="${MODE_WIDTH}x${MODE_HEIGHT}@XR24"
	modetest_plane="${plane}@${crtc}:${MODE_WIDTH}x${MODE_HEIGHT}@XR24"
	log "starting modetest scanout producer on connector $connector, CRTC $crtc, primary plane $plane"
	run_as_root modetest "${modetest_device_args[@]}" -a -s "$connector@$crtc:$modetest_mode" -P "$modetest_plane" -v >"$WORK_DIR/modetest.log" 2>&1 &
	PRODUCER_PID=$!
	sleep 3

	if ! kill -0 "$PRODUCER_PID" 2>/dev/null; then
		log "modetest exited; see $WORK_DIR/modetest.log"
		wait "$PRODUCER_PID" 2>/dev/null || modetest_rc=$?
		PRODUCER_PID=""

		if grep -Eq 'Permission denied|Atomic Commit failed|failed to find mode' "$WORK_DIR/modetest.log"; then
			{
				printf 'modetest failed while committing the test scanout.\n'
				if grep -q 'failed to find mode' "$WORK_DIR/modetest.log"; then
					printf 'Cause: the requested mode name was not found on the connector.\n'
					printf 'The Hermes connector advertises modes named "WIDTHxHEIGHT"; do not pass a -REFRESH suffix.\n'
				else
					printf 'Likely cause: another process owns DRM master for %s (e.g. KWin/Xwayland still holds the seat-tagged card).\n' "${DEVICE:-Hermes-KMS}"
				fi
				printf 'The modetest command was:\n'
				printf '  modetest %s -a -s %s -P %s -v\n' "${modetest_device_args[*]}" "$connector@$crtc:$modetest_mode" "$modetest_plane"
				printf '\nmodetest log:\n'
				sed -n '1,120p' "$WORK_DIR/modetest.log"
				printf '\n'
				printf 'If the module was loaded before the seat-ignore udev rule applied, unload and reload hermes_kms.\n'
				printf '\nOpeners:\n'
				if [ -n "$DEVICE" ] && command -v lsof >/dev/null 2>&1; then
					lsof "$DEVICE" 2>&1 || true
					if [ -n "$CONTROL_DEVICE" ] && [ "$CONTROL_DEVICE" != "$DEVICE" ]; then
						lsof "$CONTROL_DEVICE" 2>&1 || true
					fi
				else
					printf 'lsof unavailable or device path unknown\n'
				fi
			} >"$WORK_DIR/modetest-diagnostics.txt"
			log "modetest permission diagnostics written to $WORK_DIR/modetest-diagnostics.txt"
		fi
	fi

	return 0
}

if [ "$SKIP_BUILD" -eq 0 ]; then
	log "building module and control tool"
	make -C "$REPO_ROOT"
else
	[ -x "$CTL" ] || fail "$CTL is missing or not executable"
	[ -f "$KO" ] || fail "$KO is missing"
fi

load_isolated_module()
{
	log "loading hermes_kms for isolated testing (initial_enabled=1 hotplug_events=0)"
	run_as_root insmod "$KO" initial_enabled=1 hotplug_events=0 initial_width="$MODE_WIDTH" initial_height="$MODE_HEIGHT" initial_refresh_hz="$MODE_HZ"
	LOADED_BY_SCRIPT=1
	OUTPUT_READY_WITHOUT_OWNER=1
}

unload_module()
{
	run_as_root rmmod hermes_kms || fail "could not unload hermes_kms; close any compositor/Apollo session holding it, then retry"
	# Wait for the DRM nodes to drop before any reinsert.
	for _ in $(seq 1 50); do
		module_loaded || break
		sleep 0.1
	done
	module_loaded && fail "hermes_kms still loaded after rmmod; a process is holding the device open"
}

if [ "$SKIP_LOAD" -eq 0 ]; then
	if module_loaded; then
		if [ "$NO_RELOAD" -eq 1 ]; then
			# Keep whatever is running. Useful for testing the live
			# compositor-driven path, but it may be stale code or a
			# hotplug_events=1 build that KWin/Xwayland will grab.
			log "hermes_kms is already loaded; keeping it (--no-reload)"
			if module_hotplug_events_enabled; then
				log "warning: running module has hotplug_events=1; KWin may take DRM master before modetest"
			fi
		else
			# Always reload so the freshly built .ko is what runs. A module
			# left over from a previous run (even an already-isolated one)
			# would otherwise execute stale code, and a hotplug_events=1
			# build lets KWin/Xwayland take DRM master and EBUSY-block
			# hermes-kmsctl. Pass --no-reload to keep the running module.
			log "hermes_kms is already loaded; reloading to pick up the on-disk module"
			unload_module
			load_isolated_module
		fi
	else
		load_isolated_module
	fi
else
	log "skipping module load"
fi

log "waiting for Hermes-KMS DRM device"
if [ -z "$DEVICE" ]; then
	DEVICE="$(discover_hermes_drm_node card || true)"
	if [ -n "$DEVICE" ]; then
		log "using Hermes-KMS modeset device $DEVICE"
	fi
fi

warn_if_seat_tagged "$DEVICE"

if [ -z "$CONTROL_DEVICE" ]; then
	if [ -n "$DEVICE" ]; then
		CONTROL_DEVICE="$DEVICE"
		log "using Hermes-KMS control device $CONTROL_DEVICE"
	fi
fi

version_output="$(wait_for_device)" || fail "could not find/open Hermes-KMS DRM device"
printf '%s\n' "$version_output"
check_loaded_uapi_version "$version_output"

log "checking capabilities"
caps_output="$(run_ctl caps)"
printf '%s\n' "$caps_output"
require_cap "$caps_output" frame_acquire
require_cap "$caps_output" dmabuf_export
require_cap "$caps_output" zero_copy_target
require_cap "$caps_output" frame_wait
require_cap "$caps_output" sync_file
require_cap "$caps_output" metrics

start_output_owner || fail "could not keep Hermes-KMS output session open"
status_output="$(run_ctl status)"
printf '%s\n' "$status_output"
current_sequence="$(printf '%s\n' "$status_output" | get_value frame_sequence)"
[ -n "$current_sequence" ] || current_sequence=0

log "trying DMA-BUF frame acquire"
frame_output="$(try_acquire_dmabuf)" && frame_rc=0 || frame_rc=$?
printf '%s\n' "$frame_output"

if [ "$frame_rc" -eq 0 ]; then
	validate_frame_output "$frame_output"
	run_vaapi_import_check
	log "metrics after successful frame acquire"
	run_ctl metrics
	exit 0
fi

if printf '%s\n' "$frame_output" | grep -q 'No data available'; then
	log "no scanout framebuffer is active yet"
	if start_custom_producer || start_modetest_producer; then
		log "waiting for frame sequence newer than $current_sequence"
		if ! run_ctl wait "$current_sequence" 3000; then
			log "WAIT_FRAME did not observe a new frame before timeout"
		fi

		log "retrying DMA-BUF frame acquire after producer startup"
		frame_output="$(try_acquire_dmabuf)" && frame_rc=0 || frame_rc=$?
		printf '%s\n' "$frame_output"

		if [ "$frame_rc" -eq 0 ]; then
			validate_frame_output "$frame_output"
			run_vaapi_import_check
			log "metrics after successful frame acquire"
			run_ctl metrics
			exit 0
		fi
	fi

	log "diagnostics after missing framebuffer"
	run_ctl diagnose || true

	inconclusive "driver exposes the DMA-BUF/zero-copy target caps, but no active framebuffer was available to export"
fi

fail "frame acquire failed: $frame_output"
