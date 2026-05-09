# O2Ring sync FSM reorder + miss-handling

**Date:** 2026-05-08
**Status:** Draft (awaiting user review)

## Problem

Automatic ring sync at end-of-CPAP-session has been failing 100% of the
time for many days. The user has manually triggered sync from the web UI
every morning.

Root cause is FSM ordering. Today:

```
IDLE → LISTENING → ACQUIRING → UPLOADING → RELEASING → O2RING_SYNC → COOLDOWN → IDLE
```

`O2RING_SYNC` runs **after** the multi-minute SMB upload of the night's
DATALOG. By the time `RELEASING` completes, the user has long since
removed the ring and the ring's awake-after-"END" timeout has elapsed.
The ring is asleep, not advertising, and unreachable.

This is not a tight timing race. It's a serial-ordering bug.

## Goals

- Auto-sync the ring most mornings without user intervention.
- Don't make CPAP upload wait on ring sync (CPAP data is not
  time-sensitive — it sits on the SD card).
- Provide an unobtrusive cue (HA lamp blink) when auto-sync misses, so
  the user can wake the ring with a button press without opening any
  app.

## Non-goals

- Eliminate every miss. Manual web-UI trigger remains the fallback.
- Continuous incremental sync during the night. Bigger architectural
  change; out of scope here.
- A new always-on hardware sidecar (Xiao C3 etc). Existing FYSETC SD
  WIFI PRO firmware already does both jobs — it just has them in the
  wrong order.

## Key facts (from existing code and prior research)

- The OxyII GATT service that exposes file pull is only useful **after**
  a recording is finalized ("END" displayed on the ring). Mid-recording,
  the ring advertises but with a stripped GATT layout.
- The ring stays awake (advertising/connectable) for some timeout after
  "END" before sleeping. Exact timeout unknown — empirically short
  enough that the current FSM order misses it 100% of the time but long
  enough to catch with a 1–2 minute scan window if scanning starts
  promptly.
- BLE/WiFi coex requires WiFi modem sleep (`SAVE_MID`,
  `WIFI_PS_MIN_MODEM`). Already in place — see `main.cpp setup()`.
- `O2RING_SYNC` needs WiFi (for SMB `.vld` upload) but does not need SD
  bus ownership.

Memory note `project_o2ring_advertising_window.md` corrects an earlier
"3-second post-finalization window" theory: the ring advertises any
time it is awake (on-finger or button-pressed), not just for a 3s
window. Code in `scripts/o2r_pair/pull_v2_mtu23.py` predates that
correction and over-narrows the window in its comments.

## Design

### 1. FSM reorder (the core fix)

Move `O2RING_SYNC` ahead of `ACQUIRING`:

```
IDLE → LISTENING → O2RING_SYNC → ACQUIRING → UPLOADING → RELEASING → COOLDOWN → IDLE
                                                                    └─ O2RING_RETRY (on initial miss) ─┘
```

`O2RING_SYNC` fires the moment `LISTENING` confirms CPAP-idle. CPAP
upload runs after, with no time pressure.

### 2. Scan window

- Bounded BLE scan during `O2RING_SYNC`. Default 120 s, configurable
  via a new `O2RING_SCAN_TIMEOUT_SECONDS` key in `config.txt`.
- High-duty active scan, filter on the OxyII-mode advertisement (logic
  already exists in `O2RingSync` — unchanged).
- On hit: connect, run the existing READ_FILE flow, upload `.vld`
  files via the existing SMB path. No change here.
- On timeout with no OxyII match: log the miss, fire the HA cue (§3),
  and proceed to `ACQUIRING`. CPAP upload is **not** gated on ring
  sync.

The 120 s window assumes the user removes the ring within roughly a
minute of hitting CPAP stop. This matches the user's morning routine:
CPAP-stop → take off mask → take off ring → 10s ring countdown → "END".

### 3. Miss handling — Home Assistant cue

When the primary scan window times out without a sync, fire a single
HTTP POST to a configurable HA webhook so the user gets an ambient
visual cue (e.g., bedside lamp double-blink).

New `config.txt` keys (both optional; empty `HA_WEBHOOK_URL` disables
the cue entirely):

- `HA_WEBHOOK_URL` — full URL, e.g.
  `http://homeassistant.local:8123/api/webhook/ring_sync_miss`
- `HA_WEBHOOK_TIMEOUT_MS` — default `3000`

Payload (JSON, ~80 bytes):

```json
{ "event": "ring_sync_miss",
  "device": "<deviceSegment>",
  "ts": <unix_epoch> }
```

POST failure is logged and otherwise ignored. The HA-side automation
(blink the lamp) is out of scope for the firmware.

The user-side mental model: see flicker → button-press the ring →
(per the corrected memory) ring becomes connectable again → extended
retry window catches it (§4).

### 4. Extended retry window

After a primary miss, continue scanning at lower duty for an
additional N minutes (default 5, configurable as
`O2RING_RETRY_WINDOW_MINUTES`).

Two implementation options, in order of simplicity:

- **v1 (recommended):** retry runs as a new `O2RING_RETRY` state
  inserted after `COOLDOWN`, before returning to `IDLE`. Sequential —
  no concurrency with SMB upload. Downside: COOLDOWN can take several
  minutes, so retry begins fairly late. Mitigated by the user
  triggering the ring with a button press after seeing the HA flicker;
  the ring's awake window resets on button press.
- **v2 (later, if v1 isn't enough):** retry runs in parallel with
  `UPLOADING`/`RELEASING`. Avoids the COOLDOWN delay but introduces
  WiFi+BLE coex complexity. Defer until v1 is validated.

On retry success, optionally fire a second HA webhook event
(`ring_sync_recovered`) so the user gets a confirmation cue. Tunable
later — start without it.

### 5. Status surface

`O2RingStatus` (NVS-backed, surfaced via `/api/o2ring-status`) already
reports last-sync metadata. Extend the result enum with explicit miss
reasons:

- `MISS_SCAN_TIMEOUT` (no OxyII ad seen)
- `MISS_CONNECT_FAIL` (ad seen, connect failed)
- `MISS_READ_FAIL` (connected, READ_FILE flow failed)

Existing dashboard card consumes the new values without UI changes
beyond a label-table update.

## Open questions / risks

- **Q (verify during testing):** Does a button press on a finalized,
  not-currently-recording ring re-open the OxyII window? Memory says
  yes; field-validate during the first week of real use.
- **Q (verify during testing):** How long does the ring actually stay
  awake post-"END" without a button press? Determines whether 120 s is
  conservative or tight.
- **Risk:** BLE/WiFi coex during the primary scan. Not new — coex is
  already required by the existing post-RELEASING `O2RING_SYNC` —
  `SAVE_MID` modem-sleep stays in place. v1 retry is sequential, so it
  doesn't add coex surface.
- **Risk:** Adding a 120 s primary scan in the common case delays CPAP
  upload by ~2 minutes. Acceptable: CPAP data is not time-sensitive.
- **Risk:** A misconfigured `HA_WEBHOOK_URL` (wrong host, dead HA)
  blocks the FSM for `HA_WEBHOOK_TIMEOUT_MS`. Mitigated by the 3 s
  default.

## Out of scope

- Continuous incremental ring sync during the night.
- Always-on Xiao C3 / dedicated sidecar hardware.
- Wake-on-motion (accelerometer trigger when ring is set down).
- Ring battery / firmware version surfacing in the status card.

## Testing

- Native unit tests:
  - Extend `test_o2ring_sync/` for scan-timeout → miss → proceed path.
  - New `test_ha_webhook/` for POST construction, timeout, empty-URL
    disables, non-2xx tolerance.
- Manual end-to-end:
  - Build, flash, single-night auto-sync. Verify reorder via serial
    log state transitions.
  - Force-miss test: take ring off and place out of BLE range during
    `LISTENING`; verify HA POST fires; bring ring in range during
    retry window with a button press; verify recovery.

## Implementation outline

1. Reorder transitions in `main.cpp` and `UploadFSM.h` (move
   `O2RING_SYNC` ahead of `ACQUIRING`).
2. Add `O2RING_SCAN_TIMEOUT_SECONDS` config key, thread through
   `O2RingSync::run()`.
3. Add `HA_WEBHOOK_URL` and `HA_WEBHOOK_TIMEOUT_MS` config keys.
4. New small helper (`HaWebhook` in `src/`) — single HTTP POST with
   timeout. Logs result; never throws.
5. Wire the miss path in `O2RingSync` (or its caller in `main.cpp`) to
   fire `HaWebhook::fire()` and stash a structured miss reason on
   `O2RingStatus`.
6. Add `O2RING_RETRY` state + transition out of `COOLDOWN`. Reuse the
   `O2RingSync` entry point; just a longer/lower-duty scan budget.
7. Update `docs/CONFIG_REFERENCE.md` and `docs/FEATURE_FLAGS.md`.
8. Tests as above.

## Acceptance

Auto-sync succeeds on most mornings without manual web-UI intervention.
On miss nights, the bedside lamp flickers and a button-press recovery
catches the ring within 5 minutes.
