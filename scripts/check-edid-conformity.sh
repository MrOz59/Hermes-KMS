#!/usr/bin/env bash
# Hermes-KMS synthetic EDID conformity check.
#
# tests/edid.c pins the bytes the generator produces, which proves the generator
# is self-consistent. It cannot prove the result is what EDID 1.4 and CTA-861
# actually asked for, because the test and the generator share an author. So run
# a real parser over the same bytes and pin its verdict instead.
#
# Every variant must pass edid-decode --check. Its remaining warnings are
# recorded so a changed parser verdict is visible during review.
#
# Needs edid-decode (v4l-utils). SKIPs cleanly when it is absent, so this can sit
# in "make check" on machines and CI images that do not have it.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="$REPO/tests/edid-conformity.expected"
UPDATE=0

usage()
{
	cat >&2 <<EOF
usage: ${0##*/} [--update]

  --update  Rewrite the recorded baseline from this machine's edid-decode
            instead of comparing against it. Review the resulting diff: a
            new failure line is a regression, not a baseline update.
EOF
}

case "${1:-}" in
--update) UPDATE=1 ;;
-h | --help)
	usage
	exit 0
	;;
"") ;;
*)
	usage
	exit 1
	;;
esac

if ! command -v edid-decode >/dev/null 2>&1; then
	printf 'EDID conformity: SKIP (edid-decode not installed)\n'
	exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/hermes-kms-edid-conformity.XXXXXX")"
cleanup() { rm -rf -- "$TMP"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
	"$REPO/tests/edid-dump.c" -o "$TMP/edid-dump"

# Reduce edid-decode's report to just its verdict: one sorted "kind|block|text"
# line per finding. Sorting makes the comparison independent of the order the
# parser happens to walk the blocks in, and dropping the decode section keeps
# the baseline from churning every time edid-decode reformats how it prints a
# timing it already understood.
normalize()
{
	awk '
		/^Warnings:/ { section = "warning"; next }
		/^Failures:/ { section = "failure"; next }
		/^EDID conformity:/ { section = ""; next }
		section == "" { next }
		/^[^ \t]/ { block = $0; sub(/:$/, "", block); next }
		/^[ \t]+[^ \t]/ {
			line = $0
			sub(/^[ \t]+/, "", line)
			if (line != "")
				printf "%s|%s|%s\n", section, block, line
		}
	' | LC_ALL=C sort
}

# Each variant is a configuration a user can actually load. Keeping the
# base-only case here as well is what makes the HDR findings attributable: the
# difference between the two is exactly what enabling HDR introduces.
variants=(
	"base-defaults:"
	"base-ten-bit:--color-depth 10"
	"generic:--generic"
	"profile-720p:--width 1280 --height 720 --refresh 60 --preferred-width 1280 --preferred-height 720 --preferred-refresh 60"
	"hdr-eight-bit:--hdr"
	"hdr-ten-bit:--hdr --color-depth 10"
	"generic-hdr:--generic --hdr"
	"profile-720p-hdr:--width 1280 --height 720 --refresh 60 --preferred-width 1280 --preferred-height 720 --preferred-refresh 60 --hdr"
)

generated="$TMP/generated"
: > "$generated"

# Pull one variant's findings back out of the combined report, so the two tiers
# below can reason about a single configuration without re-running the parser.
findings_for()
{
	awk -v want="### $1 " '
		index($0, want) == 1 { inside = 1; next }
		/^### / { inside = 0 }
		inside && /\|/ { print }
	' "$generated"
}

for variant in "${variants[@]}"; do
	name="${variant%%:*}"
	args="${variant#*:}"

	# shellcheck disable=SC2086 # args is a deliberate argument list, not a path
	"$TMP/edid-dump" $args > "$TMP/$name.bin"

	printf '### %s (%s bytes)\n' "$name" "$(stat -c%s "$TMP/$name.bin")" \
		>> "$generated"

	# Capture the complete report so failures can be explained with the
	# parser's findings rather than only its exit status.
	edid-decode --check < "$TMP/$name.bin" > "$TMP/$name.report" 2>&1 || true
	normalize < "$TMP/$name.report" >> "$generated"

	# The plain decode is a separate artifact from the conformity report: it
	# describes the bytes, where --check applies a rule set. Tier 1 below
	# reads this one precisely because it does not depend on which rules a
	# given edid-decode version happens to implement.
	edid-decode < "$TMP/$name.bin" > "$TMP/$name.decode" 2>&1 || true
	if ! grep -q '^EDID conformity: PASS' "$TMP/$name.report" ||
	   findings_for "$name" | grep -q '^failure|'; then
		printf 'FAIL: %s synthetic EDID is non-conformant\n' "$name" >&2
		findings_for "$name" >&2
		exit 1
	fi
done

if [ "$UPDATE" -eq 1 ]; then
	{
		printf '# Recorded edid-decode --check findings for the synthetic EDID.\n'
		printf '#\n'
		printf '# Regenerate with scripts/check-edid-conformity.sh --update.\n'
		printf '# A new "failure|" line is a regression to fix or justify in\n'
		printf '# docs/driver-design.md, not a baseline to rubber-stamp.\n'
		printf '#\n'
		printf '# Recorded with: %s\n' "$(edid-decode --version 2>&1 | head -1)"
		printf '\n'
		cat "$generated"
	} > "$BASELINE"
	printf 'EDID conformity: baseline updated (%s)\n' "$BASELINE"
	exit 0
fi

if [ ! -f "$BASELINE" ]; then
	printf 'FAIL: no baseline at %s; create it with %s --update\n' \
		"$BASELINE" "${0##*/}" >&2
	exit 1
fi

# Strip the file's own header comments ("# " or a bare "#") and blank lines,
# but keep the "### variant" markers: they are what attributes each finding to
# a configuration, so a finding moving between variants has to show as a diff.
# ---------------------------------------------------------------------------
# Tier 1: what the parser DECODES, always enforced.
#
# This tier deliberately reads edid-decode's plain decode rather than its
# --check findings. Which conformity rules a given edid-decode implements varies
# by version -- an older build simply does not report some of them -- so a
# finding set, or even a count of findings, is not a stable thing to assert
# across machines. What the parser reports about the bytes themselves is far
# more stable, and it is also the thing worth guarding: that the EDID says what
# the driver meant it to say.
#
# These block and value names come from CTA-861 and have been spelled this way
# in edid-decode for a long time, but they are not guaranteed forever. If one
# drifts, this fails loudly and names the version it saw, which is the right
# failure mode -- unlike silently comparing rule sets that do not exist on the
# machine running the check.
# ---------------------------------------------------------------------------
tier1_failed=0

expect_in_decode()
{
	local name="$1" what="$2" needle="$3"

	if grep -qF -- "$needle" "$TMP/$name.decode"; then
		printf 'ok: %s: %s\n' "$name" "$what"
	else
		printf 'FAIL: %s: %s (no "%s" in the decode)\n' \
			"$name" "$what" "$needle" >&2
		tier1_failed=1
	fi
}

expect_absent_from_decode()
{
	local name="$1" what="$2" needle="$3"

	if grep -qF -- "$needle" "$TMP/$name.decode"; then
		printf 'FAIL: %s: %s ("%s" present, expected absent)\n' \
			"$name" "$what" "$needle" >&2
		tier1_failed=1
	else
		printf 'ok: %s: %s\n' "$name" "$what"
	fi
}

# A checksum error makes a block vanish silently from every parser that reads
# it, which is the one EDID mistake with no visible symptom.
for variant in "${variants[@]}"; do
	name="${variant%%:*}"
	if grep -qiE 'checksum.*(error|mismatch)' "$TMP/$name.report"; then
		printf 'FAIL: %s has an EDID checksum error\n' "$name" >&2
		tier1_failed=1
	fi
done
[ "$tier1_failed" -eq 0 ] &&
	printf 'ok: no checksum errors in any variant\n'

for variant in "${variants[@]}"; do
	name="${variant%%:*}"
	args="${variant#*:}"

	case "$args" in
	*--hdr*)
		# The whole point of hdr_enable: a parser must find both data
		# blocks, with PQ and BT2020 RGB specifically.
		expect_in_decode "$name" "CTA-861 extension block present" \
			"CTA-861 Extension Block"
		expect_in_decode "$name" "HDR Static Metadata block" \
			"HDR Static Metadata Data Block"
		expect_in_decode "$name" "advertises SMPTE ST2084 (PQ)" \
			"SMPTE ST2084"
		expect_in_decode "$name" "Colorimetry block" \
			"Colorimetry Data Block"
		expect_in_decode "$name" "advertises BT2020 RGB" "BT2020RGB"
		;;
	*)
		# And with it off, none of that may appear: the disabled path has
		# to stay a plain single-block EDID.
		expect_absent_from_decode "$name" "no CTA extension block" \
			"CTA-861 Extension Block"
		expect_absent_from_decode "$name" "no HDR metadata block" \
			"HDR Static Metadata Data Block"
		;;
	esac
done

[ "$tier1_failed" -eq 0 ] || exit 1

# ---------------------------------------------------------------------------
# Tier 2: the exact finding set. Only meaningful against the edid-decode that
# recorded it -- a different version rewords findings it already had, which
# would be reported as a regression it is not. So compare when the versions
# agree and say plainly why not when they do not.
# ---------------------------------------------------------------------------
recorded_version="$(sed -n 's/^# Recorded with: //p' "$BASELINE")"
running_version="$(edid-decode --version 2>&1 | head -1)"

if [ "$recorded_version" != "$running_version" ]; then
	printf 'EDID conformity: PARTIAL (version-independent checks passed)\n'
	printf '  baseline recorded with: %s\n' "$recorded_version"
	printf '  this machine has:       %s\n' "$running_version"
	printf '  exact finding set not compared; re-record on this version with\n'
	printf '  scripts/check-edid-conformity.sh --update to compare it here too\n'
	exit 0
fi

# Strip the file's own header comments ("# " or a bare "#") and blank lines,
# but keep the "### variant" markers: they are what attributes each finding to
# a configuration, so a finding moving between variants has to show as a diff.
grep -v '^#\([[:space:]]\|$\)' "$BASELINE" | grep -v '^[[:space:]]*$' \
	> "$TMP/expected" || true
grep -v '^[[:space:]]*$' "$generated" > "$TMP/actual" || true

if ! diff -u "$TMP/expected" "$TMP/actual" > "$TMP/diff"; then
	printf 'FAIL: edid-decode findings changed\n\n' >&2
	sed -n '3,$p' "$TMP/diff" >&2
	cat >&2 <<EOF

A "+failure|" line means this change made the EDID less conformant.
A "-" line means a finding was fixed -- update the baseline and say so in
docs/driver-design.md.
EOF
	exit 1
fi

printf 'EDID conformity: PASS (%d findings match the recorded baseline)\n' \
	"$(grep -c '|' "$TMP/actual" || true)"
