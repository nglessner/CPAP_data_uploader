# AirSense 11 smoke fixture

**Date:** 2026-05-10

## Problem

Today, confidence that a firmware build "will actually work in a CPAP" rests on two
things: native unit tests (`pio test -e native`, fast but mocked) and physically
inserting the FYSETC card into the user's AS11 (slow, expensive when it fails
mid-night). There is no middle tier — no way to flash a candidate build, point it
at a realistic DATALOG, and watch it run end-to-end before committing the card to
a live therapy night.

The user wants a manual bench-level smoke that exercises the real firmware
binary against a realistic DATALOG tree and reports success on the dashboard.
Not a regression suite, not CI, not a chaos rig — a sanity check before risky
merges or releases.

## Goal

A single happy-path bench test that verifies:

1. The flashed firmware boots, reads `config.txt`, and joins WiFi.
2. The FSM advances through `LISTENING → ACQUIRING → UPLOADING → RELEASING`.
3. A realistic DATALOG tree on the microSD lands on a local SMB target with its
   directory structure intact.
4. The dashboard at `http://cpap.local` reports the upload as complete.
5. A second power-cycle uploads zero new files (persisted dedup state holds).

The smoke is **self-contained inside the `CPAP_data_uploader` repo** — no
dependency on `/opt/homelab/sleep` so the fixture survives if the fork is ever
cloned standalone.

## Non-goals

- CI integration. Bench-only, run manually.
- Scripted PASS/FAIL exit code. `ls` and eyeball are sufficient.
- Brownout / power-supply behavior (cannot be exercised without a CPAP).
- Smart-mode DAT3 detection logic (no host is driving the bus).
- `ACQUIRING` / `RELEASING` race versus a real CPAP host.
- CLOUD / SleepHQ upload path. SMB only for V1.
- O2Ring sync path. Separate fixture, out of scope.
- Multi-night cycles, scheduled-mode timing windows, parser-robustness fuzzing.
- Backfilling fixtures from new firmware variants.

If any of these become priorities later, the natural extension points are
documented in **Future work** below.

## Architecture

All new artifacts live under `CPAP_data_uploader/test/smoke/`:

```
test/smoke/
├── README.md                    Operator walkthrough.
├── prep-card.sh                 Populates the microSD before insertion.
├── config.template.txt          Skeleton config.txt with placeholders.
├── .env.smoke.example           Template for gitignored creds.
├── docker-compose.smoke.yml     Local samba target.
├── fixture/
│   └── DATALOG/<yyyymm>/<yyyymmdd>/*.edf
└── .smb-share/                  (gitignored) bind mount; uploads land here.
```

### Fixture data

A real AS11 night, copied from `tests/fixtures/20260413/` in the Sleep repo, is
committed verbatim under `test/smoke/fixture/`. The uploader repo owns its
copy; we deliberately do not symlink to the Sleep repo. Expected size is a few
MB.

If the AS11 firmware ever changes the on-disk format in a way our parser cares
about, the fixture is regenerated once and committed.

### `prep-card.sh <sd-mount-point>`

1. Verifies `<sd-mount-point>` exists, is writable, and looks like a fresh
   microSD (refuses if it contains unrelated files outside `DATALOG/`,
   `config.txt`, and the upload-state files).
2. Copies `fixture/DATALOG/` into the card root.
3. Renders `config.template.txt` to `<card>/config.txt`, substituting values
   from `.env.smoke`.
4. Deletes any pre-existing `.upload_state.v2.*` files so dedup starts fresh.
5. Prints a summary of what changed.

### `.env.smoke`

Gitignored. The operator copies `.env.smoke.example` to `.env.smoke` once and
fills in:

- `WIFI_SSID`, `WIFI_PASSWORD`
- `SMB_HOST` — the dev box's LAN IP (must be reachable from the WiFi the FYSETC
  joins).
- `SMB_SHARE` — defaults to `cpap-smoke`.
- `SMB_USER`, `SMB_PASS` — must match `docker-compose.smoke.yml`.

`*.env.smoke` is added to `.gitignore`.

### `docker-compose.smoke.yml`

A `dperson/samba` container on host networking, exposing one writable share
`cpap-smoke` bind-mounted to `./.smb-share/`. Auth is required (no anonymous
access) so the firmware exercises the auth path. Credentials are dev-only and
documented as such — not production credentials.

## Operator workflow

1. Build & flash firmware (existing commands).
2. `docker compose -f test/smoke/docker-compose.smoke.yml up -d`.
3. Pull microSD from the FYSETC, mount via USB SD reader on the dev box.
4. `./test/smoke/prep-card.sh /media/$USER/<sdmount>`.
5. Reinsert microSD into the FYSETC, power on (programming board USB).
6. Open serial monitor for logs (`stty + cat` per CLAUDE.md).
7. Open `http://cpap.local` and watch the dashboard until the upload completes.
8. `ls test/smoke/.smb-share/cpap-smoke/DATALOG/...` — confirm the tree matches
   the fixture.
9. Power-cycle the card. Confirm a second run uploads zero new files.

### Pass criteria (eyeball-level)

- Dashboard reports upload success.
- File tree on SMB matches the fixture's DATALOG layout (same paths, same
  filenames, file sizes within EDF-padding tolerance).
- Second run reports 0 files uploaded (dedup is honored).

## What this catches

- Build-time regressions that prevent firmware boot or WiFi join.
- Config-parse regressions for the most common SMB-only config.
- SMB auth, connection, file-write, and directory-create paths in libsmb2.
- Dashboard rendering of upload progress + completion.
- Persistent dedup state across reboots.

## What this misses (acknowledged)

- Brownout / power-supply behavior (real AS11 problem; untestable without a CPAP).
- Smart-mode `LISTENING` correctness (no DAT3 activity is being driven).
- `ACQUIRING` / `RELEASING` race vs a real host.
- CLOUD / SleepHQ upload path.
- O2Ring sync path.
- Parser robustness on weird firmware variants (CSL/EVE/PLD edge cases).
- WiFi disconnect / SMB target unavailable error paths.

## Risks & mitigations

- **WiFi credentials in `.env.smoke`** — gitignored, never committed; operator
  responsibility. `.env.smoke*` is added to `.gitignore`.
- **SMB credentials shared between operator and container** — fixed dev-only
  creds in the compose file are documented as not-production.
- **Fixture data size growth** — one night of summary + signal data is expected
  to land in single-digit MB. Move to git LFS only if a future fixture exceeds
  10 MB.
- **Stale dedup state masking real failures** — `prep-card.sh` always clears
  `.upload_state.v2.*`, so each run starts from a deterministic state.
- **`prep-card.sh` writing to the wrong device** — the script refuses to write
  to a mount point that contains unrelated files, and prints the resolved path
  before any mutation.

## Future work (out of scope for V1)

- Scripted PASS/FAIL exit code on the operator workflow.
- Hardware-in-the-loop variant that swaps the programming board for a USB SD
  reader, so DAT3 activity is naturally driven by the host.
- Mid-run chaos: host script writes additional DATALOG files during the
  `ACQUIRING` window to exercise bus contention.
- Multi-night cycle harness (compressed wall-clock) to exercise cooldown and
  scheduled-mode windows.
- CI-on-hardware: a FYSETC card permanently wired to the gitea-runner host.
- CLOUD / SleepHQ smoke variant.
- O2Ring smoke fixture (would reuse the same harness shape; the ring already
  has working direct-BLE pull).
