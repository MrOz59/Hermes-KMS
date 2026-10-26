#!/usr/bin/env bash
# Check source/default package versions without evaluating a PKGBUILD.
set -euo pipefail
cd "$(dirname "$0")/.."

version=
for component in MAJOR MINOR PATCH; do
  value=$(awk -v key="HERMES_KMS_DRIVER_$component" '
    $1 == "#define" && $2 == key { value=$3; count++ }
    END {
      if (count != 1 || value !~ /^(0|[1-9][0-9]*)$/) exit 1
      print value
    }' kernel/hermes-kms/hermes_kms.c)
  version="${version:+$version.}$value"
done

grep -Fqx "PACKAGE_VERSION=\"$version\"" dkms.conf
test "$(grep -c '^PACKAGE_VERSION=' dkms.conf)" -eq 1
grep -Fqx "pkgver=$version" PKGBUILD
test "$(grep -c '^pkgver=' PKGBUILD)" -eq 1
grep -Fq "## [$version]" CHANGELOG.md
test -s "docs/releases/v$version.md"

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != "v$version" ]; }; then
  printf 'release tag must be v%s\n' "$version" >&2
  exit 1
fi
printf '%s\n' "$version"
