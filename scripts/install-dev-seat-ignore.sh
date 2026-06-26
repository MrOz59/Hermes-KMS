#!/usr/bin/env bash
set -euo pipefail

OLD_RULE_PATH=/etc/udev/rules.d/70-hermes-kms-ignore-seat.rules
RULE_PATH=/etc/udev/rules.d/99-hermes-kms-ignore-seat.rules
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RULE_SRC="$REPO_ROOT/udev/99-hermes-kms-ignore-seat.rules"

if [ "$(id -u)" -ne 0 ]; then
	printf 'error: run as root: sudo %s\n' "$0" >&2
	exit 1
fi

rm -f "$OLD_RULE_PATH"
install -m 0644 "$RULE_SRC" "$RULE_PATH"

udevadm control --reload-rules
udevadm trigger --subsystem-match=drm --action=change || true

printf 'installed %s\n' "$RULE_PATH"
printf 'Hermes-KMS primary nodes are set to group video, mode 0660, with uaccess for non-root control.\n'
printf 'If KWin already opened Hermes-KMS, log out and back in once, then run sudo rmmod hermes_kms.\n'
printf '\n'
printf 'NOTE: this rule is for ISOLATED testing (e.g. modetest) only. It removes the\n'
printf 'seat assignment, which also stops KWin/GNOME from adopting the Hermes output\n'
printf '(the compositor only manages GPUs on its own seat). For real compositor-driven\n'
printf 'streaming, run scripts/remove-dev-seat-ignore.sh and reload the module.\n'
