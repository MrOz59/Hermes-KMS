#!/usr/bin/env bash
set -u

PROJECT_DIR="${PROJECT_DIR:-$HOME/Projects/Hermes-KMS}"
SCRIPT_NAME="$(basename "$0")"
TIMESTAMP="$(date +'%Y%m%d-%H%M%S')"
LOG_ROOT="$PROJECT_DIR/build/tty-zero-copy-test"
LOG_DIR="$LOG_ROOT/$TIMESTAMP"

KEEP_LOADED="${KEEP_LOADED:-0}"
STOP_DISPLAY_MANAGER="${STOP_DISPLAY_MANAGER:-1}"
KILL_GRAPHICAL_LEFTOVERS="${KILL_GRAPHICAL_LEFTOVERS:-1}"

mkdir -p "$LOG_DIR"

MAIN_LOG="$LOG_DIR/main.log"
COMMAND_LOG="$LOG_DIR/commands.log"
DRI_BEFORE_LOG="$LOG_DIR/dri-before.log"
DRI_AFTER_STOP_LOG="$LOG_DIR/dri-after-stop.log"
DRI_AFTER_TEST_LOG="$LOG_DIR/dri-after-test.log"
LSMOD_LOG="$LOG_DIR/lsmod.log"
DMESG_BEFORE_LOG="$LOG_DIR/dmesg-before.log"
DMESG_AFTER_LOG="$LOG_DIR/dmesg-after.log"
SYSTEM_LOG="$LOG_DIR/system.log"
TEST_STDOUT_LOG="$LOG_DIR/test-driver-zero-copy.stdout.log"
TEST_STDERR_LOG="$LOG_DIR/test-driver-zero-copy.stderr.log"

log() {
    echo "[$SCRIPT_NAME] $*" | tee -a "$MAIN_LOG"
}

run() {
    echo "\$ $*" | tee -a "$COMMAND_LOG" "$MAIN_LOG"
    "$@" >>"$MAIN_LOG" 2>&1
    local code=$?
    echo "[exit=$code] $*" | tee -a "$COMMAND_LOG" "$MAIN_LOG"
    return "$code"
}

run_to_file() {
    local outfile="$1"
    shift
    echo "\$ $*" | tee -a "$COMMAND_LOG" "$MAIN_LOG"
    "$@" >"$outfile" 2>&1
    local code=$?
    echo "[exit=$code] $* -> $outfile" | tee -a "$COMMAND_LOG" "$MAIN_LOG"
    return "$code"
}

safe_pkill() {
    local pattern="$1"
    log "killing leftovers matching: $pattern"
    sudo pkill -9 -f "$pattern" >>"$MAIN_LOG" 2>&1 || true
}

detect_display_manager() {
    systemctl status display-manager --no-pager >>"$SYSTEM_LOG" 2>&1 || true

    local dm_name
    dm_name="$(systemctl show display-manager -p Id --value 2>/dev/null || true)"

    if [[ -z "$dm_name" || "$dm_name" == "display-manager.service" ]]; then
        if systemctl is-active --quiet sddm 2>/dev/null; then
            dm_name="sddm.service"
        elif systemctl is-active --quiet gdm 2>/dev/null; then
            dm_name="gdm.service"
        elif systemctl is-active --quiet lightdm 2>/dev/null; then
            dm_name="lightdm.service"
        else
            dm_name="display-manager.service"
        fi
    fi

    echo "$dm_name"
}

collect_dri_state() {
    local outfile="$1"

    {
        echo "===== date ====="
        date

        echo
        echo "===== tty/session ====="
        tty || true
        loginctl || true

        echo
        echo "===== /dev/dri ====="
        ls -lah /dev/dri 2>/dev/null || true

        echo
        echo "===== lsmod hermes ====="
        lsmod | grep -E 'hermes|evdi|drm' || true

        echo
        echo "===== lsof /dev/dri ====="
        sudo lsof /dev/dri/card* /dev/dri/renderD* 2>/dev/null || true

        echo
        echo "===== fuser /dev/dri ====="
        sudo fuser -v /dev/dri/card* /dev/dri/renderD* 2>/dev/null || true

        echo
        echo "===== drm status ====="
        for s in /sys/class/drm/card*-*/status; do
            [ -e "$s" ] || continue
            echo "$s: $(cat "$s" 2>/dev/null || true)"
        done

        echo
        echo "===== drm drivers ====="
        for c in /sys/class/drm/card*; do
            [ -e "$c" ] || continue
            echo "== $c =="
            readlink -f "$c/device/driver/module" 2>/dev/null || true
            cat "$c/device/uevent" 2>/dev/null || true
        done

        echo
        echo "===== processes ====="
        ps aux | grep -Ei 'hermes|apollo|sunshine|gamescope|kwin|xwayland|xorg|plasmashell|sddm|gdm|lightdm|wayland' | grep -v grep || true
    } >"$outfile" 2>&1
}

print_summary() {
    echo
    echo "============================================================"
    echo "Hermes-KMS TTY zero-copy test finished"
    echo "Logs saved in:"
    echo "$LOG_DIR"
    echo
    echo "Most useful files:"
    echo "  $MAIN_LOG"
    echo "  $TEST_STDOUT_LOG"
    echo "  $TEST_STDERR_LOG"
    echo "  $DRI_BEFORE_LOG"
    echo "  $DRI_AFTER_STOP_LOG"
    echo "  $DRI_AFTER_TEST_LOG"
    echo "  $DMESG_AFTER_LOG"
    echo
    echo "After returning to KDE, open with:"
    echo "  dolphin \"$LOG_DIR\""
    echo
    echo "Or inspect quickly:"
    echo "  less \"$MAIN_LOG\""
    echo "  less \"$TEST_STDOUT_LOG\""
    echo "  less \"$TEST_STDERR_LOG\""
    echo "============================================================"
    echo
}

cleanup_on_error() {
    local code="$1"

    log "cleanup_on_error called with exit code $code"

    collect_dri_state "$DRI_AFTER_TEST_LOG"
    dmesg -T >"$DMESG_AFTER_LOG" 2>&1 || true
    lsmod >"$LSMOD_LOG" 2>&1 || true

    if [[ "$KEEP_LOADED" != "1" ]]; then
        log "KEEP_LOADED=0, trying to stop Hermes-KMS owner processes and unload module"
        sudo pkill -f hermes-kms-output-owner >>"$MAIN_LOG" 2>&1 || true
        sudo pkill -f hermes-kms >>"$MAIN_LOG" 2>&1 || true
        sudo rmmod hermes_kms >>"$MAIN_LOG" 2>&1 || true
    else
        log "KEEP_LOADED=1, leaving hermes_kms loaded"
    fi

    print_summary
    exit "$code"
}

trap 'cleanup_on_error $?' EXIT

cd "$PROJECT_DIR" || {
    echo "Could not cd into PROJECT_DIR=$PROJECT_DIR"
    exit 1
}

log "starting Hermes-KMS TTY zero-copy test"
log "project dir: $PROJECT_DIR"
log "log dir: $LOG_DIR"
log "KEEP_LOADED=$KEEP_LOADED"
log "STOP_DISPLAY_MANAGER=$STOP_DISPLAY_MANAGER"
log "KILL_GRAPHICAL_LEFTOVERS=$KILL_GRAPHICAL_LEFTOVERS"

if [[ "$EUID" -eq 0 ]]; then
    log "do not run this script as root directly; run as normal user with sudo available"
    exit 1
fi

if ! command -v sudo >/dev/null 2>&1; then
    log "sudo not found"
    exit 1
fi

if [[ ! -x "$PROJECT_DIR/scripts/test-driver-zero-copy.sh" ]]; then
    log "missing executable: $PROJECT_DIR/scripts/test-driver-zero-copy.sh"
    log "try: chmod +x $PROJECT_DIR/scripts/test-driver-zero-copy.sh"
    exit 1
fi

log "refreshing sudo credentials"
sudo -v

log "collecting initial state"
collect_dri_state "$DRI_BEFORE_LOG"
dmesg -T >"$DMESG_BEFORE_LOG" 2>&1 || true

DISPLAY_MANAGER="$(detect_display_manager)"
log "detected display manager: $DISPLAY_MANAGER"

if [[ "$STOP_DISPLAY_MANAGER" == "1" ]]; then
    log "stopping display manager"
    sudo systemctl stop "$DISPLAY_MANAGER" >>"$MAIN_LOG" 2>&1 || sudo systemctl stop display-manager >>"$MAIN_LOG" 2>&1 || true
    sleep 3
else
    log "STOP_DISPLAY_MANAGER=0, not stopping display manager"
fi

if [[ "$KILL_GRAPHICAL_LEFTOVERS" == "1" ]]; then
    log "killing graphical leftovers"
    safe_pkill 'Xwayland'
    safe_pkill 'kwin_wayland'
    safe_pkill 'plasmashell'
    safe_pkill 'Xorg'
    safe_pkill 'gamescope'
    safe_pkill 'sunshine'
    safe_pkill 'apollo'
else
    log "KILL_GRAPHICAL_LEFTOVERS=0, not killing graphical leftovers"
fi

log "killing old Hermes-KMS test processes"
sudo pkill -f hermes-kms-output-owner >>"$MAIN_LOG" 2>&1 || true
sudo pkill -f hermes-kmsctl >>"$MAIN_LOG" 2>&1 || true

log "trying to unload old hermes_kms module"
sudo rmmod hermes_kms >>"$MAIN_LOG" 2>&1 || true

sleep 1

log "collecting state after stopping graphical session"
collect_dri_state "$DRI_AFTER_STOP_LOG"

log "checking for remaining /dev/dri/card2 openers"
if sudo lsof /dev/dri/card2 >>"$MAIN_LOG" 2>&1; then
    log "WARNING: /dev/dri/card2 still has openers. The test may fail with Permission denied."
else
    log "/dev/dri/card2 has no openers or does not exist yet"
fi

log "running test-driver-zero-copy.sh"
set +e
"$PROJECT_DIR/scripts/test-driver-zero-copy.sh" --keep-loaded >"$TEST_STDOUT_LOG" 2>"$TEST_STDERR_LOG"
TEST_CODE=$?
set -e

log "test-driver-zero-copy.sh exit code: $TEST_CODE"

log "copying generated test logs if present"
if [[ -d "$PROJECT_DIR/build/hermes-kms-zero-copy" ]]; then
    cp -a "$PROJECT_DIR/build/hermes-kms-zero-copy" "$LOG_DIR/hermes-kms-zero-copy-copy" >>"$MAIN_LOG" 2>&1 || true
fi

log "collecting final state"
collect_dri_state "$DRI_AFTER_TEST_LOG"
dmesg -T >"$DMESG_AFTER_LOG" 2>&1 || true
lsmod >"$LSMOD_LOG" 2>&1 || true

if [[ "$KEEP_LOADED" != "1" ]]; then
    log "KEEP_LOADED=0, stopping Hermes-KMS owner processes and unloading module"
    sudo pkill -f hermes-kms-output-owner >>"$MAIN_LOG" 2>&1 || true
    sudo pkill -f hermes-kms >>"$MAIN_LOG" 2>&1 || true
    sleep 1
    sudo rmmod hermes_kms >>"$MAIN_LOG" 2>&1 || true
else
    log "KEEP_LOADED=1, leaving hermes_kms loaded for inspection"
fi

trap - EXIT

print_summary

exit "$TEST_CODE"