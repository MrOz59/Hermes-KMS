#!/usr/bin/env bash
# Hermes-KMS synthetic EDID conformity check.
#
# tests/edid.c pins the bytes the generator produces, which proves the generator
# is self-consistent. It cannot prove the result is what EDID 1.4 and CTA-861
# actually asked for, because the test and the generator share an author. So run
# a real parser over the same bytes and pin its verdict instead.
#
# The point is not a clean bill of health -- the EDID does not currently pass
# edid-decode --check, and docs/driver-design.md records why. The point is that
# the set of findings is a tracked artifact: a change that quietly adds a new
# conformity failure shows up here as a diff instead of surviving into a release
# and being discovered by someone else's parser.
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
	"hdr-eight-bit:--hdr"
	"hdr-ten-bit:--hdr --color-depth 10"
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

# How many findings enabling HDR adds, measured by diffing two runs of the *same*
# edid-decode binary. This is the assertion worth running anywhere: it compares
# like with like, so it holds whatever version of edid-decode is installed, while
# the exact wording of any individual finding does not.
hdr_introduced_count()
{
	local base hdr

	base="$(findings_for base-defaults | wc -l)"
	hdr="$(findings_for hdr-eight-bit | wc -l)"
	printf '%s\n' "$((hdr - base))"
}

for variant in "${variants[@]}"; do
	name="${variant%%:*}"
	args="${variant#*:}"

	# shellcheck disable=SC2086 # args is a deliberate argument list, not a path
	"$TMP/edid-dump" $args > "$TMP/$name.bin"

	printf '### %s (%s bytes)\n' "$name" "$(stat -c%s "$TMP/$name.bin")" \
		>> "$generated"

	# edid-decode exits non-zero when conformity fails, which is the expected
	# verdict here -- the findings are the output we want, not an error. So
	# capture the report and judge it below rather than letting its exit
	# status end the run.
	edid-decode --check < "$TMP/$name.bin" > "$TMP/$name.report" 2>&1 || true
	normalize < "$TMP/$name.report" >> "$generated"
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
		printf '# hdr-introduced-findings: %s\n' "$(hdr_introduced_count)"
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
# Tier 1: invariants that hold whatever edid-decode version is installed.
# These are what make the check worth running in CI, where the parser version is
# whatever the runner image ships and will not match the recording below.
# ---------------------------------------------------------------------------
tier1_failed=0

# A checksum error makes a block vanish silently from every parser that reads
# it, which is the one EDID mistake with no visible symptom. No variant may
# have one.
for variant in "${variants[@]}"; do
	name="${variant%%:*}"
	if grep -qiE 'checksum.*(error|mismatch)' "$TMP/$name.report"; then
		printf 'FAIL: %s has an EDID checksum error\n' "$name" >&2
		tier1_failed=1
	fi
done
[ "$tier1_failed" -eq 0 ] &&
	printf 'ok: no checksum errors in any variant\n'

expected_introduced="$(sed -n 's/^# hdr-introduced-findings: //p' "$BASELINE" 2>/dev/null)"
actual_introduced="$(hdr_introduced_count)"
if [ -n "$expected_introduced" ] &&
	[ "$actual_introduced" != "$expected_introduced" ]; then
	printf 'FAIL: enabling HDR now adds %s findings, expected %s\n' \
		"$actual_introduced" "$expected_introduced" >&2
	printf '      (both counts come from this machine'"'"'s own edid-decode, so\n' >&2
	printf '       this is a real change in the EDID, not a version difference)\n' >&2
	tier1_failed=1
else
	printf 'ok: enabling HDR adds %s findings, as recorded\n' "$actual_introduced"
fi

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
