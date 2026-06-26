#!/usr/bin/env bash
set -euo pipefail

OLD_RULE_PATH=/etc/udev/rules.d/70-hermes-kms-ignore-seat.rules
RULE_PATH=/etc/udev/rules.d/99-hermes-kms-ignore-seat.rules

if [ "$(id -u)" -ne 0 ]; then
	printf 'error: run as root: sudo %s\n' "$0" >&2
	exit 1
fi

rm -f "$OLD_RULE_PATH" "$RULE_PATH"
udevadm control --reload-rules
udevadm trigger --subsystem-match=drm --action=change || true

printf 'removed %s and %s\n' "$OLD_RULE_PATH" "$RULE_PATH"
