#!/usr/bin/env bash
# Verifies the glibc symbol-version ceiling of a packaged build.
#
# Usage:
#   tools/check_glibc_baseline.sh <staging-dir> <max-glibc>
#   e.g. tools/check_glibc_baseline.sh build/release/runtime 2.39
#
# Every ELF file under <staging-dir> is scanned for the glibc symbol versions
# its *undefined* symbols bind to (readelf --dyn-syms).  A binary linked
# against a newer glibc refuses to start on an older one ("version `GLIBC_2.39'
# not found"), so the highest version found is the artifact's true minimum
# Linux -- even though glibc itself is never bundled.
#
# Baseline policy: the release pipeline builds on a specific Ubuntu image, and
# the ceiling passed to this check is that image's glibc version, documented in
# docs/releases/README.md.  When the build image moves, the ceiling in the
# release workflow moves with it: the point of the check is that the baseline
# cannot change silently, e.g. by a dependency that starts using a newer symbol.
#
# Exit status:
#   0  every scanned file stays within the ceiling
#   1  a file requires a glibc newer than the ceiling
#   2  usage error / missing tools
set -euo pipefail

usage="usage: tools/check_glibc_baseline.sh <staging-dir> <max-glibc> (e.g. 2.39)"

if [ "$#" -ne 2 ]; then
  echo "$usage" >&2
  exit 2
fi

STAGE_DIR="$1"
CEILING="${2#GLIBC_}"
[ -d "$STAGE_DIR" ] || {
  echo "error: '$STAGE_DIR' is not a directory" >&2
  exit 2
}
if ! command -v readelf >/dev/null 2>&1; then
  echo "error: readelf not found (install binutils)" >&2
  exit 2
fi

is_elf() {
  [ -f "$1" ] || return 1
  [ "$(od -An -N4 -tx1 -- "$1" | tr -d ' \n')" = "7f454c46" ]
}

# True when version $1 <= version $2 (dotted numeric, sort -V semantics).
version_le() {
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" = "$1" ]
}

# The highest GLIBC_x.y among the file's undefined (imported) symbols.
max_requirement() {
  readelf --dyn-syms --wide -- "$1" 2>/dev/null |
    awk '$7 == "UND" { print $8 }' |
    sed -n 's/.*@\(GLIBC_[0-9][0-9.]*\).*/\1/p' |
    sort -Vu |
    tail -n 1
}

worst=""
worst_file=""
failed=0
scanned=0
while IFS= read -r -d '' f; do
  is_elf "$f" || continue
  scanned=$((scanned + 1))
  required="$(max_requirement "$f")"
  [ -n "$required" ] || continue
  if [ -z "$worst" ] || ! version_le "${required#GLIBC_}" "${worst#GLIBC_}"; then
    worst="$required"
    worst_file="$f"
  fi
  if ! version_le "${required#GLIBC_}" "$CEILING"; then
    echo "error: ${f#"$STAGE_DIR"/} requires $required, above the baseline GLIBC_$CEILING" >&2
    failed=1
  fi
done < <(find "$STAGE_DIR" -type f -print0)

if [ "$failed" -ne 0 ]; then
  echo "glibc baseline check FAILED (ceiling GLIBC_$CEILING; $scanned ELF files scanned)" >&2
  exit 1
fi

if [ -z "$worst" ]; then
  echo "glibc baseline check: OK (no glibc symbol requirements found in $scanned ELF files)"
else
  echo "glibc baseline check: OK (highest requirement $worst in ${worst_file#"$STAGE_DIR"/} <= GLIBC_$CEILING; $scanned ELF files scanned)"
fi
