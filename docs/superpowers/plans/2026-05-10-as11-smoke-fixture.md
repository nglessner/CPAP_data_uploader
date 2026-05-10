# AS11 Smoke Fixture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a self-contained manual bench smoke under `test/smoke/` that lets an operator populate a microSD with a realistic AS11 DATALOG and verify the firmware uploads it to a local SMB target.

**Architecture:** Six new files plus a `.gitignore` update. A committed single-night fixture (~3 MB), a docker-compose'd samba container, a `prep-card.sh` populator, a config template + env example, and an operator README. No firmware changes.

**Tech Stack:** Bash + envsubst, docker-compose, dperson/samba, shellcheck (lint).

**Spec:** `docs/superpowers/specs/2026-05-10-as11-smoke-fixture-design.md`

**Working tree:** This plan is intended to run inside the `docs/as11-smoke-fixture` branch worktree at `.worktrees/smoke-fixture-spec/`. All paths below are relative to the repo root.

---

## File Structure

```
.gitignore                                          (modify)
test/smoke/
├── README.md                                       (create)
├── prep-card.sh                                    (create, +x)
├── config.template.txt                             (create)
├── .env.smoke.example                              (create)
├── docker-compose.smoke.yml                        (create)
└── fixture/
    ├── Identification.crc                          (create, copied)
    ├── Identification.json                         (create, copied)
    ├── STR.edf                                     (create, copied)
    ├── SETTINGS/
    │   ├── CurrentSettings.crc                     (create, copied)
    │   └── CurrentSettings.json                    (create, copied)
    └── DATALOG/20260413/
        ├── 20260413_221123_CSL.edf                 (create, copied)
        ├── 20260413_221123_EVE.edf                 (create, copied)
        ├── 20260413_221131_CSL.edf                 (create, copied)
        ├── 20260413_221131_EVE.edf                 (create, copied)
        ├── 20260413_221134_BRP.edf                 (create, copied)
        ├── 20260413_221134_PLD.edf                 (create, copied)
        └── 20260413_221134_SA2.edf                 (create, copied)
```

Responsibility per file:

- `.gitignore` — block credentials and runtime bind-mount before any other file lands.
- `fixture/...` — checked-in copy of one real AS11 night plus the root metadata files; never modified at runtime.
- `config.template.txt` — `${VAR}` placeholders for everything `.env.smoke` provides.
- `.env.smoke.example` — operator-copies-and-fills template; the real `.env.smoke` is gitignored.
- `docker-compose.smoke.yml` — single samba container on host networking, fixed dev-only creds.
- `prep-card.sh` — the only piece with logic. Validates target, copies fixture, renders config, clears upload state.
- `README.md` — six-step operator workflow.

---

### Task 1: Gitignore entries

**Files:**
- Modify: `.gitignore` (append a `test/smoke/` block)

- [ ] **Step 1: Inspect current `.gitignore` tail**

Run: `tail -5 .gitignore`
Expected: shows the "Firmware backups" comment block and the rest of the file. Confirms there is no existing `test/smoke/` section.

- [ ] **Step 2: Append the smoke-fixture block**

Add to the end of `.gitignore`:

```gitignore

# AS11 smoke fixture (test/smoke/)
test/smoke/.env.smoke
test/smoke/.smb-share/
```

- [ ] **Step 3: Verify the entries match**

Run: `git check-ignore -v test/smoke/.env.smoke test/smoke/.smb-share/junk 2>&1`
Expected: both paths print, each citing the line numbers we just added (something like `.gitignore:NN:test/smoke/.env.smoke   test/smoke/.env.smoke`).

- [ ] **Step 4: Commit**

```bash
git add .gitignore
git commit -m "chore: gitignore smoke fixture creds and runtime bind mount"
```

---

### Task 2: Copy real-night fixture

**Files:**
- Create: `test/smoke/fixture/Identification.crc`
- Create: `test/smoke/fixture/Identification.json`
- Create: `test/smoke/fixture/STR.edf`
- Create: `test/smoke/fixture/SETTINGS/CurrentSettings.crc`
- Create: `test/smoke/fixture/SETTINGS/CurrentSettings.json`
- Create: `test/smoke/fixture/DATALOG/20260413/*.edf` (7 files)

Source: `/opt/homelab/sleep/tests/fixtures/20260413/` (single night `20260413` only — other dates would balloon the fixture past 1 GB).

- [ ] **Step 1: Make fixture directories**

```bash
mkdir -p test/smoke/fixture/SETTINGS \
         test/smoke/fixture/DATALOG/20260413
```

- [ ] **Step 2: Copy the root metadata files**

```bash
SRC=/opt/homelab/sleep/tests/fixtures/20260413
cp "$SRC/Identification.crc"  test/smoke/fixture/
cp "$SRC/Identification.json" test/smoke/fixture/
cp "$SRC/STR.edf"             test/smoke/fixture/
cp "$SRC/SETTINGS/CurrentSettings.crc"  test/smoke/fixture/SETTINGS/
cp "$SRC/SETTINGS/CurrentSettings.json" test/smoke/fixture/SETTINGS/
```

- [ ] **Step 3: Copy the single-night DATALOG**

```bash
SRC=/opt/homelab/sleep/tests/fixtures/20260413
cp "$SRC/DATALOG/20260413/"*.edf test/smoke/fixture/DATALOG/20260413/
```

- [ ] **Step 4: Verify fixture shape and size**

Run: `find test/smoke/fixture -type f | sort && du -sh test/smoke/fixture`
Expected:
- 12 files total (3 root files + 2 SETTINGS + 7 DATALOG EDFs)
- `du -sh` reports ~3 MB

- [ ] **Step 5: Commit**

```bash
git add test/smoke/fixture
git commit -m "test(smoke): add AS11 single-night fixture (DATALOG 20260413)"
```

---

### Task 3: docker-compose for local samba target

**Files:**
- Create: `test/smoke/docker-compose.smoke.yml`

- [ ] **Step 1: Write the compose file**

```yaml
# Local SMB target for the AS11 smoke fixture.
# Dev-only credentials — do not reuse outside this fixture.
#
# Bring up:  docker compose -f test/smoke/docker-compose.smoke.yml up -d
# Tear down: docker compose -f test/smoke/docker-compose.smoke.yml down
# Inspect:   ls test/smoke/.smb-share/cpap-smoke/

services:
  smoke-samba:
    image: dperson/samba:latest
    container_name: cpap-smoke-samba
    network_mode: host
    restart: unless-stopped
    command: >
      -u "smoke;smokepass"
      -s "cpap-smoke;/share;yes;no;no;smoke"
      -p
      -W
    volumes:
      - ./.smb-share:/share
```

- [ ] **Step 2: Validate the compose file parses**

Run: `docker compose -f test/smoke/docker-compose.smoke.yml config >/dev/null && echo OK`
Expected: prints `OK`. Any YAML or schema error fails here.

- [ ] **Step 3: Bring it up and probe**

```bash
mkdir -p test/smoke/.smb-share
docker compose -f test/smoke/docker-compose.smoke.yml up -d
sleep 3
smbclient -L //127.0.0.1 -U smoke%smokepass -m SMB3 2>&1 | grep -E '^\s*cpap-smoke'
```

Expected: prints a line containing `cpap-smoke` from the share list. If `smbclient` is not installed, install it (`sudo apt-get install -y smbclient`) and re-run.

- [ ] **Step 4: Tear it back down**

```bash
docker compose -f test/smoke/docker-compose.smoke.yml down
```

Expected: container `cpap-smoke-samba` removed.

- [ ] **Step 5: Commit**

```bash
git add test/smoke/docker-compose.smoke.yml
git commit -m "test(smoke): add local samba compose target"
```

---

### Task 4: Config template and env example

**Files:**
- Create: `test/smoke/config.template.txt`
- Create: `test/smoke/.env.smoke.example`

- [ ] **Step 1: Write `config.template.txt`**

Modeled on `docs/config.txt.example.smb`, with `${VAR}` placeholders for everything `.env.smoke` provides:

```text
# CPAP Data Uploader — smoke fixture config
# Generated by test/smoke/prep-card.sh from this template.
# Do NOT commit a rendered config.txt — it contains plaintext credentials
# until the firmware migrates them to NVS on first boot.

WIFI_SSID = ${WIFI_SSID}
WIFI_PASSWORD = ${WIFI_PASSWORD}
HOSTNAME = ${HOSTNAME}

ENDPOINT_TYPE = SMB
ENDPOINT = //${SMB_HOST}/${SMB_SHARE}
ENDPOINT_USER = ${SMB_USER}
ENDPOINT_PASSWORD = ${SMB_PASS}

UPLOAD_MODE = smart
UPLOAD_START_HOUR = 8
UPLOAD_END_HOUR = 22

INACTIVITY_SECONDS = 125
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10

GMT_OFFSET_HOURS = ${GMT_OFFSET_HOURS}

CPU_SPEED_MHZ = 240
WIFI_TX_PWR = high
WIFI_PWR_SAVING = none

# Force credentials to plaintext for smoke testing so the operator can re-prep
# the card without manually clearing NVS between runs.
STORE_CREDENTIALS_PLAIN_TEXT = true
```

- [ ] **Step 2: Write `.env.smoke.example`**

```bash
# Copy to test/smoke/.env.smoke and fill in.
# .env.smoke is gitignored; .env.smoke.example is committed.

# --- WiFi the FYSETC card joins ------------------------------------------
WIFI_SSID="your-2.4ghz-ssid"
WIFI_PASSWORD="your-wifi-password"
HOSTNAME="cpap-smoke"

# --- Local SMB target (matches docker-compose.smoke.yml) -----------------
# SMB_HOST must be the LAN IP of the host running docker-compose, reachable
# from the WiFi the FYSETC joins. 127.0.0.1 will NOT work — the firmware is
# on a different device.
SMB_HOST="192.168.1.10"
SMB_SHARE="cpap-smoke"
SMB_USER="smoke"
SMB_PASS="smokepass"

# --- Timezone offset (hours from GMT) ------------------------------------
GMT_OFFSET_HOURS="-5"
```

- [ ] **Step 3: Verify the template renders cleanly**

```bash
set -a; . test/smoke/.env.smoke.example; set +a
envsubst < test/smoke/config.template.txt | head -20
```

Expected: prints the first 20 lines of a fully-substituted `config.txt`, with `WIFI_SSID = your-2.4ghz-ssid`, `ENDPOINT = //192.168.1.10/cpap-smoke`, etc. No unsubstituted `${...}` tokens visible. (Run `envsubst < test/smoke/config.template.txt | grep -F '${'` — should print nothing.)

- [ ] **Step 4: Commit**

```bash
git add test/smoke/config.template.txt test/smoke/.env.smoke.example
git commit -m "test(smoke): add config template and env example"
```

---

### Task 5: `prep-card.sh` — validate args and reject bad paths

The script is built up in three task slices (Tasks 5, 6, 7) so each piece is independently verifiable. After Task 5 the script can refuse to run on a bad target; after Task 6 it can populate a card; after Task 7 it clears stale upload state too.

**Files:**
- Create: `test/smoke/prep-card.sh` (+x)

- [ ] **Step 1: Write the skeleton + arg/target validation**

```bash
#!/usr/bin/env bash
# prep-card.sh — populate a microSD for the AS11 firmware smoke fixture.
#
# Usage: ./test/smoke/prep-card.sh <sd-mount-point>
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
      sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
```

- [ ] **Step 2: Make it executable**

```bash
chmod +x test/smoke/prep-card.sh
```

- [ ] **Step 3: Lint it**

Run: `shellcheck test/smoke/prep-card.sh`
Expected: exits 0 with no findings. If `shellcheck` is not installed, `sudo apt-get install -y shellcheck`.

- [ ] **Step 4: Verify "no args" rejection**

Run: `./test/smoke/prep-card.sh; echo "exit=$?"`
Expected: stderr line `prep-card.sh: usage: prep-card.sh <sd-mount-point> [--force]` and `exit=1`.

- [ ] **Step 5: Verify "non-directory" rejection**

```bash
./test/smoke/prep-card.sh /etc/hostname; echo "exit=$?"
```

Expected: stderr `prep-card.sh: not a directory: /etc/hostname` and `exit=1`.

- [ ] **Step 6: Verify "missing .env.smoke" rejection on a fresh dir**

```bash
TMP=$(mktemp -d)
./test/smoke/prep-card.sh "$TMP"; echo "exit=$?"
rm -rf "$TMP"
```

Expected: stderr `prep-card.sh: missing .../test/smoke/.env.smoke (copy from .env.smoke.example)` and `exit=1`.

- [ ] **Step 7: Verify "unrelated files" rejection**

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke   # temporary; remove after
TMP=$(mktemp -d)
touch "$TMP/random.txt"
./test/smoke/prep-card.sh "$TMP"; echo "exit=$?"
./test/smoke/prep-card.sh "$TMP" --force; echo "exit=$?"
rm -rf "$TMP"
rm test/smoke/.env.smoke
```

Expected:
- First call: stderr lists `random.txt` under "unrelated files" and `exit=1`.
- Second call (`--force`): prints `prep-card.sh: target OK: ...` and `exit=0`.

- [ ] **Step 8: Verify "clean dir" happy path**

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke
TMP=$(mktemp -d)
./test/smoke/prep-card.sh "$TMP"; echo "exit=$?"
rm -rf "$TMP"
rm test/smoke/.env.smoke
```

Expected: `prep-card.sh: target OK: ...` and `exit=0`.

- [ ] **Step 9: Commit**

```bash
git add test/smoke/prep-card.sh
git commit -m "test(smoke): prep-card.sh arg + target validation"
```

---

### Task 6: `prep-card.sh` — fixture copy + config render

Extends the script to actually populate the validated target.

**Files:**
- Modify: `test/smoke/prep-card.sh`

- [ ] **Step 1: Replace the trailing placeholder block with the population logic**

Replace the last two lines of `prep-card.sh`:

```bash
echo "prep-card.sh: target OK: $TARGET"
echo "prep-card.sh: (validation only — population steps land in later tasks)"
```

with:

```bash
echo "prep-card.sh: target OK: $TARGET"

# Load the env file. shellcheck source=/dev/null
set -a
. "$ENV_FILE"
set +a

# Sanity-check that nothing in the template was left unset.
required=(WIFI_SSID WIFI_PASSWORD HOSTNAME SMB_HOST SMB_SHARE \
          SMB_USER SMB_PASS GMT_OFFSET_HOURS)
for v in "${required[@]}"; do
  [[ -n "${!v:-}" ]] || die "$ENV_FILE: $v is empty or unset"
done

echo "prep-card.sh: copying fixture into $TARGET ..."
# -a preserves mode/timestamps; trailing /. copies contents not the dir itself.
cp -a "$FIXTURE_DIR/." "$TARGET/"

echo "prep-card.sh: rendering config.txt ..."
envsubst < "$TEMPLATE" > "$TARGET/config.txt"

# Refuse to leave any unsubstituted placeholders on the card.
if grep -F '${' "$TARGET/config.txt" >/dev/null; then
  die "rendered config.txt still contains \${...} placeholders — check $ENV_FILE"
fi

echo "prep-card.sh: done."
echo "  fixture files: $(find "$TARGET" -type f | wc -l)"
echo "  config.txt:    $TARGET/config.txt"
```

- [ ] **Step 2: Lint again**

Run: `shellcheck test/smoke/prep-card.sh`
Expected: exits 0.

- [ ] **Step 3: End-to-end on a fake target**

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke
TMP=$(mktemp -d)
./test/smoke/prep-card.sh "$TMP"
echo "---"
find "$TMP" -type f | sort
echo "---"
head -15 "$TMP/config.txt"
rm -rf "$TMP"
rm test/smoke/.env.smoke
```

Expected:
- 13 files printed (12 fixture + `config.txt`).
- `config.txt` head shows `WIFI_SSID = your-2.4ghz-ssid`, `ENDPOINT = //192.168.1.10/cpap-smoke`, etc., with no `${...}` tokens.

- [ ] **Step 4: Missing-required-var rejection**

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke
sed -i 's/^WIFI_SSID=.*/WIFI_SSID=""/' test/smoke/.env.smoke
TMP=$(mktemp -d)
./test/smoke/prep-card.sh "$TMP"; echo "exit=$?"
rm -rf "$TMP"
rm test/smoke/.env.smoke
```

Expected: stderr `prep-card.sh: .../test/smoke/.env.smoke: WIFI_SSID is empty or unset` and `exit=1`.

- [ ] **Step 5: Commit**

```bash
git add test/smoke/prep-card.sh
git commit -m "test(smoke): prep-card.sh copy fixture + render config"
```

---

### Task 7: `prep-card.sh` — clear stale upload state

The firmware writes `/.upload_state.v2{,.smb,.smb.log,.cloud,.cloud.log,.log}` at the SD card root for dedup. The smoke must start each run from a deterministic state so a previously-prepped card cannot mask a regression.

**Files:**
- Modify: `test/smoke/prep-card.sh`

- [ ] **Step 1: Insert the state-clear step before the fixture copy**

Insert this block immediately above the existing `echo "prep-card.sh: copying fixture into $TARGET ..."` line:

```bash
echo "prep-card.sh: clearing any stale upload-state files ..."
shopt -s nullglob
removed=("$TARGET"/.upload_state.v2*)
if (( ${#removed[@]} > 0 )); then
  rm -f -- "${removed[@]}"
  printf '  removed: %s\n' "${removed[@]##*/}"
fi
shopt -u nullglob
```

- [ ] **Step 2: Lint**

Run: `shellcheck test/smoke/prep-card.sh`
Expected: exits 0.

- [ ] **Step 3: Verify it clears the expected files and tolerates absence**

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke
TMP=$(mktemp -d)
# Seed a "previously prepped" card by faking the state files.
touch "$TMP/.upload_state.v2.smb" \
      "$TMP/.upload_state.v2.smb.log" \
      "$TMP/.upload_state.v2"
./test/smoke/prep-card.sh "$TMP"
# Second run on a freshly-populated card should NOT print "removed:" lines
# for missing files (nullglob suppresses the empty expansion).
./test/smoke/prep-card.sh "$TMP" --force | grep -c 'removed:'
rm -rf "$TMP"
rm test/smoke/.env.smoke
```

Expected:
- First `prep-card.sh` run prints `removed: .upload_state.v2.smb`, etc.
- Second run's `grep -c 'removed:'` prints `0`. (It does print the "clearing" header line — the count is for the per-file `removed:` lines only.)

- [ ] **Step 4: Commit**

```bash
git add test/smoke/prep-card.sh
git commit -m "test(smoke): prep-card.sh clears stale upload-state files"
```

---

### Task 8: Operator README

**Files:**
- Create: `test/smoke/README.md`

- [ ] **Step 1: Write the README**

```markdown
# AS11 Smoke Fixture

A manual bench smoke that flashes a candidate firmware build, hands it a
realistic AS11 DATALOG on a real microSD, and verifies the upload lands on a
local SMB target with the dashboard reporting success.

This is **smoke-only**: a single happy path. It does not exercise brownout
behavior, smart-mode DAT3 detection, bus contention, the CLOUD upload path,
or the O2Ring sync. See `docs/superpowers/specs/2026-05-10-as11-smoke-fixture-design.md`
for the full scope.

## What you need

- A FYSETC SD WIFI PRO with its programming board (USB power + serial).
- The microSD that goes into the FYSETC's onboard slot, plus a USB SD reader
  to mount it on the dev box.
- Docker (for the local samba container).
- A 2.4 GHz WiFi network the FYSETC can join, and the dev box must be
  reachable from it on the SMB port.

## One-time setup

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke
$EDITOR test/smoke/.env.smoke   # fill in WiFi + SMB_HOST
```

`SMB_HOST` must be the LAN IP of this machine, **not 127.0.0.1** — the
firmware lives on a different device and cannot reach your loopback.

## Run the smoke

1. **Build and flash the firmware**

   ```bash
   ./build_upload.sh build pico32
   pio run -e pico32 -t upload
   ```

2. **Bring up the local SMB target**

   ```bash
   mkdir -p test/smoke/.smb-share
   docker compose -f test/smoke/docker-compose.smoke.yml up -d
   ```

3. **Pull the microSD out of the FYSETC and mount it on the dev box**

   Use a USB SD reader. Note the mount point (commonly `/media/$USER/<label>`).

4. **Populate the card**

   ```bash
   ./test/smoke/prep-card.sh /media/$USER/<sdmount>
   ```

   The script refuses to write to a directory containing files outside the
   fixture set. Use `--force` if you are certain the target is the SD card.

5. **Reinsert the microSD into the FYSETC and power on**

   Programming-board USB is fine for the smoke. Watch the serial monitor:

   ```bash
   stty -F /dev/ttyUSB0 115200 raw -echo
   cat /dev/ttyUSB0
   ```

6. **Open the dashboard and verify**

   In a browser: `http://cpap-smoke.local` (or whatever `HOSTNAME` you set).
   Wait for the dashboard to report upload complete.

   Cross-check on the host:

   ```bash
   find test/smoke/.smb-share/cpap-smoke -type f | sort
   ```

   Should show the same 12 files as `test/smoke/fixture/`.

7. **Dedup check (optional)**

   Power-cycle the card and watch the dashboard — the second run should
   report 0 files uploaded.

## Pass criteria

- Dashboard reports upload success.
- File tree on SMB matches `test/smoke/fixture/` (same paths, same names,
  file sizes within EDF-padding tolerance).
- Second power-cycle uploads zero new files.

## Tear down

```bash
docker compose -f test/smoke/docker-compose.smoke.yml down
rm -rf test/smoke/.smb-share
```
```

- [ ] **Step 2: Render-check the markdown locally**

Run: `head -40 test/smoke/README.md`
Expected: prints the rendered header through the "What you need" section without escape errors.

- [ ] **Step 3: Commit**

```bash
git add test/smoke/README.md
git commit -m "test(smoke): operator README"
```

---

### Task 9: Final integration dry-run

**Files:** none (verification only).

- [ ] **Step 1: Full dry-run on a temp dir, end to end**

```bash
cp test/smoke/.env.smoke.example test/smoke/.env.smoke
docker compose -f test/smoke/docker-compose.smoke.yml config >/dev/null
shellcheck test/smoke/prep-card.sh
TMP=$(mktemp -d)
./test/smoke/prep-card.sh "$TMP"
echo "--- card contents ---"
find "$TMP" -type f | sort
echo "--- config.txt head ---"
head -12 "$TMP/config.txt"
echo "--- unsubstituted tokens (should be empty) ---"
grep -F '${' "$TMP/config.txt" || echo "(none)"
rm -rf "$TMP"
rm test/smoke/.env.smoke
```

Expected:
- shellcheck clean.
- `docker compose ... config` exits 0 with no output.
- 13 files listed under "card contents".
- `config.txt` head looks like a real config with substituted values.
- "unsubstituted tokens" prints `(none)`.

- [ ] **Step 2: Push the branch**

```bash
git push -u origin docs/as11-smoke-fixture
```

- [ ] **Step 3: Open PR against `main` (uploader fork, not upstream)**

```bash
gh pr create \
  --repo nglessner/CPAP_data_uploader \
  --base main \
  --head docs/as11-smoke-fixture \
  --title "test: AS11 bench smoke fixture" \
  --body "$(cat <<'BODY'
## Summary

- Adds `test/smoke/` — a manual bench smoke for the firmware against a
  realistic AS11 DATALOG and a local SMB target.
- Self-contained: committed fixture (one night, ~3 MB), local samba via
  docker-compose, populator script, operator README.
- No firmware changes.

## Spec

`docs/superpowers/specs/2026-05-10-as11-smoke-fixture-design.md`

## Test plan

- [ ] Run `./test/smoke/prep-card.sh` against a real microSD with the FYSETC
  card in its programming board.
- [ ] Flash a fresh `pico32` build, insert card, watch dashboard for upload
  complete.
- [ ] Confirm `test/smoke/.smb-share/cpap-smoke/` mirrors `test/smoke/fixture/`.
- [ ] Power-cycle and confirm second run uploads zero files.
BODY
)"
```

Expected: PR URL printed. Targets the `nglessner/CPAP_data_uploader` fork (per repo memory: never default to upstream `amanuense/`).

---

## Self-review notes

**Spec coverage:**
- Goals 1–5 (boot/config/FSM/files/dedup) — covered by README pass criteria + Task 7 state-clear.
- Self-contained — Task 2 commits fixture in-repo; no symlinks. ✓
- Out-of-scope items — explicitly NOT addressed (no DAT3 sim, no CI). ✓
- `.gitignore` block — Task 1. ✓
- All six artifact files — Tasks 2–8. ✓

**Placeholder scan:** No "TBD" / "implement later" / unanchored "handle edge cases". All shell snippets are complete and runnable.

**Type/name consistency:**
- Env var names (`WIFI_SSID`, `SMB_HOST`, etc.) match across `.env.smoke.example`, `config.template.txt`, and the `required=` array in `prep-card.sh`. ✓
- `cpap-smoke` share name used consistently in compose, env example, and README. ✓
- File path `test/smoke/` used throughout, not `tests/smoke/` (the Sleep repo uses `tests/`; the uploader repo uses `test/` — checked existing tree). ✓
