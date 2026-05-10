#!/usr/bin/env bash
# prep-card.sh — populate a microSD for the AS11 firmware smoke fixture.
#
# Usage: ./test/smoke/prep-card.sh <sd-mount-point> [--force]
#
# The target must be a writable directory. The script refuses to write to a
# directory that contains files unrelated to the fixture so the operator does
# not lose data on a misidentified card. Run with --force to bypass the
# unrelated-files check (still requires a writable directory).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SMOKE_DIR="$REPO_ROOT/test/smoke"
FIXTURE_DIR="$SMOKE_DIR/fixture"
ENV_FILE="$SMOKE_DIR/.env.smoke"
TEMPLATE="$SMOKE_DIR/config.template.txt"

die() { echo "prep-card.sh: $*" >&2; exit 1; }

FORCE=0
TARGET=""
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=1 ;;
    -h|--help)
      sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    -*) die "unknown flag: $arg" ;;
    *)
      [[ -z "$TARGET" ]] || die "multiple targets given"
      TARGET="$arg"
      ;;
  esac
done

[[ -n "$TARGET" ]] || die "usage: prep-card.sh <sd-mount-point> [--force]"
[[ -d "$TARGET" ]] || die "not a directory: $TARGET"
[[ -w "$TARGET" ]] || die "not writable: $TARGET"
[[ -f "$ENV_FILE" ]] || die "missing $ENV_FILE (copy from .env.smoke.example)"
[[ -f "$TEMPLATE" ]] || die "missing $TEMPLATE"
[[ -d "$FIXTURE_DIR" ]] || die "missing $FIXTURE_DIR"

# Refuse if the target contains files outside the expected smoke-fixture set.
ALLOWED='^(DATALOG|SETTINGS|Identification\.(crc|json)|STR\.edf|config\.txt|\.upload_state\.v2.*|\.Trashes|\.Spotlight-V100|\.fseventsd|System Volume Information)$'
UNEXPECTED=()
while IFS= read -r -d '' entry; do
  name="$(basename "$entry")"
  [[ "$name" =~ $ALLOWED ]] || UNEXPECTED+=("$name")
done < <(find "$TARGET" -mindepth 1 -maxdepth 1 -print0)

if (( ${#UNEXPECTED[@]} > 0 )) && (( FORCE == 0 )); then
  printf 'prep-card.sh: target contains unrelated files:\n' >&2
  printf '  %s\n' "${UNEXPECTED[@]}" >&2
  die "refusing to overwrite. Re-run with --force if this really is the SD card."
fi

echo "prep-card.sh: target OK: $TARGET"
echo "prep-card.sh: (validation only — population steps land in later tasks)"
