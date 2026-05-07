# NTP sidecar manifest per copied EDF

**Date:** 2026-05-07
**Closes:** [admin/sleep#176](http://192.168.0.108:3300/admin/sleep/issues/176) (part of epic [admin/sleep#174](http://192.168.0.108:3300/admin/sleep/issues/174))

## Problem

The AirSense 11's RTC drifts independently of NTP. Ours runs ~4 minutes ahead at present. The Sleep ingest needs a per-session correction so that CPAP-recorded timestamps can be aligned with NTP-grounded clocks (the O2Ring, the host, third-party datasets) for cross-stream analysis.

Sleep-side support is already merged:

- `Sessions.ClockOffsetSec` column ([admin/sleep#175](http://192.168.0.108:3300/admin/sleep/issues/175))
- `Sleep.Ingest/cpap_clock.py` reads a sidecar JSON next to each EDF and computes the offset ([admin/sleep#177](http://192.168.0.108:3300/admin/sleep/issues/177))

The remaining piece is the firmware-side write of that sidecar at the moment the uploader copies each EDF off the SD card. That witness time, paired with the EDF header times, is what `cpap_clock.py` needs to compute the offset.

## Goal

For every per-session EDF the uploader copies to SMB, also write `<filename>.ntp.json` next to the destination EDF, capturing the wall-clock witness data:

```json
{
  "schema_version": 1,
  "ntp_observed_at": "2026-05-07T01:39:27Z",
  "fat_mtime": "2026-05-07T01:39:25Z",
  "edf_header_start": "2026-05-06T22:29:52",
  "edf_header_duration_seconds": 27300,
  "uploader_poll_interval_seconds": 30,
  "uploader_firmware_version": "v1.4.2-dev+7"
}
```

Out of scope:

- SleepHQ / WebDAV / cloud paths. Ingest only sees the SMB share, so sidecars are SMB-only.
- `/STR.edf` (root summary, rolling — header_start does not represent a single session).
- Non-EDF mandatory files (Identification.*, settings).
- Coordination with sleep-side schema bumps (the spec is at v1 and stable).
- Backfilling sidecars for previously copied EDFs.

## Field semantics

| Field | Source | Notes |
|---|---|---|
| `schema_version` | constant `1` | Forward compat marker; cpap_clock.py rejects unknown versions. |
| `ntp_observed_at` | `time(nullptr)` UTC at *start* of the EDF copy | The witness time. ISO 8601 with `Z` suffix. |
| `fat_mtime` | `f.getLastWrite()` UTC, read pre-copy | Sanity check field. Sleep-side currently ignores it but it is forensic gold. Omit if `0`. |
| `edf_header_start` | EDF bytes 168–183 (`dd.MM.yy` + `HH.mm.ss`) | Naive local time as the device wrote it; no TZ suffix. ISO 8601 naive. |
| `edf_header_duration_seconds` | EDF bytes 236–251 (num records × record duration) | Integer seconds. Rounded if record duration is fractional. |
| `uploader_poll_interval_seconds` | `Config::INACTIVITY_SECONDS` | Bounds the latency between session-end and witness time. Drives ingest's correction error budget. |
| `uploader_firmware_version` | `FIRMWARE_VERSION` macro | From `scripts/generate_version_prebuild.py`; e.g. `v1.4.2-dev+7`. |

`ntp_observed_at`, `fat_mtime`, and `uploader_firmware_version` are not in `cpap_clock.py:_REQUIRED_FIELDS` — only `ntp_observed_at`, `edf_header_start`, `edf_header_duration_seconds`, `uploader_poll_interval_seconds`, and `schema_version` are. The extras are additive metadata; sleep-side ingest silently ignores them.

## Architecture

### Insertion point

`FileUploader::uploadSingleFileSmb` (src/FileUploader.cpp:1053) is the per-file SMB copy entry. The new logic:

```
┌─────────────────────────────────────────────────────────────────────┐
│  uploadSingleFileSmb(filePath)                                      │
│  ─────────────────────────────────────────────────────────────────  │
│  1. existing pre-flight (exists, change-detect, force-flag)         │
│  2. shouldWriteSidecar = NtpSidecarWriter::isDatalogEdf(filePath)   │
│  3. if shouldWriteSidecar:                                          │
│         witness = NtpSidecarWriter::captureWitness(sd, fp, cfg)     │
│  4. existing: smbUploader->upload(...)                              │
│  5. if upload OK and witness.valid:                                 │
│         if !NtpSidecarWriter::write(*smbUploader,                   │
│                                     remotePath, witness):           │
│             return false   ← do NOT mark uploaded; retry next pass  │
│  6. existing: markFileUploaded(...)                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### `NtpSidecarWriter` (new helper)

Files: `include/NtpSidecarWriter.h`, `src/NtpSidecarWriter.cpp`. Only compiled when `ENABLE_SMB_UPLOAD` is set — otherwise reduces to no-op stubs.

**Public surface (all static):**

```cpp
struct EdfHeader {
    char startNaiveStr[20];   // "YYYY-MM-DDTHH:MM:SS" + null
    int  durationSeconds;
};

enum class SkipReason { NONE, NTP_UNSYNCED, EDF_PARSE_FAILED };

struct SidecarPayload {
    bool valid;
    SkipReason skipReason;
    time_t ntp_observed_at_unix;
    time_t fat_mtime_unix;        // 0 means "omit field"
    EdfHeader header;
    int  pollIntervalSeconds;
    const char* firmwareVersion;
};

class NtpSidecarWriter {
public:
    static bool isDatalogEdf(const String& path);
    static SidecarPayload captureWitness(fs::FS&, const String& localPath,
                                         const Config* cfg);
    static bool write(SMBUploader&, const String& remotePath,
                      const SidecarPayload&);
};
```

**Internal helpers (file-static in `.cpp`):**

```cpp
static bool parseEdfHeader(File&, EdfHeader& out);
static bool isNtpSynced(time_t now);   // now > 1704067200 (2024-01-01)
static size_t serializeJson(const SidecarPayload&, char* buf, size_t cap);
```

`serializeJson` writes to a 512-byte stack buffer; a `static_assert` bounds the worst-case payload (firmware version is the only variable-length field; capped via the FIRMWARE_VERSION format).

### Path resolution

`<edf_remote_path>.ntp.json` — sidecar lives literally next to the EDF, same convention as `Sleep.Ingest/cpap_clock.py:sidecar_path`. Example:

```
/cpap-share/DATALOG/20260507/20260507_223015_PLD.edf
/cpap-share/DATALOG/20260507/20260507_223015_PLD.edf.ntp.json
```

### `isDatalogEdf` match rules

Match (case-insensitive on extension):

- Path startswith `/DATALOG/`
- Next 8 chars are digits (the `yyyymmdd` folder)
- Followed by exactly one `/`, then a filename ending in `.edf`

Reject everything else. Specifically:

- `/STR.edf` → false (not under DATALOG)
- `/DATALOG/foo/bar.edf` → false (folder isn't 8 digits)
- `/DATALOG/20260507/.upload_state.v2.smb` → false (not .edf)
- `/DATALOG/20260507/sub/x.edf` → false (extra path segment)

## Error handling

| Situation | Detection | Behavior |
|---|---|---|
| NTP unsynced | `time(nullptr) < 1704067200` | `captureWitness` → `{valid=false, skipReason=NTP_UNSYNCED}`. EDF still uploads. Log warning. EDF marked uploaded. Sidecar absent → ingest leaves `ClockOffsetSec` NULL. |
| EDF header parse fails | <256 bytes, bad date/time format, non-numeric duration | Returns `{valid=false, skipReason=EDF_PARSE_FAILED}`. EDF still uploads. Log warning. Marked uploaded. |
| `f.getLastWrite()` returns 0 | FAT mtime unset | `fat_mtime` field omitted from JSON (sleep-side ignores it anyway). |
| Sidecar SMB write fails | `uploadRawBuffer` returns false | `uploadSingleFileSmb` returns false → state manager does NOT mark EDF uploaded → next pass retries the pair (idempotent re-upload). |
| Re-upload of same EDF | Existing flow (force=true or change detected) | Sidecar overwrites prior `.ntp.json`. New `ntp_observed_at` reflects the actual re-upload time. Acceptable — header_start/duration are immutable. |
| Cloud (SleepHQ) upload | `uploadSingleFileCloud` (different code path) | Untouched. No sidecar. |
| Mandatory files (`/STR.edf`, etc.) | `isDatalogEdf` returns false | No sidecar attempted. Normal upload. |

The asymmetry between "skip witness because NTP unsynced" (mark uploaded, no retry) and "sidecar write failed" (don't mark, retry) is deliberate. NTP-unsynced is a property of the moment that won't fix itself by retrying the same file in the same pass; sidecar-write-failed is a transient SMB error that retry can resolve.

## Build / config integration

- New `Config` accessor `int getInactivitySeconds() const` if not already exposed (existing key parser at `src/Config.cpp:234` already stores the value — confirm during impl).
- No new `config.txt` keys.
- No `platformio.ini` changes — uses existing `ENABLE_SMB_UPLOAD` flag and existing `FIRMWARE_VERSION` macro.
- No new dependencies.

## Testing

### Native unit tests

New directory `test/test_ntp_sidecar/test_ntp_sidecar.cpp` (per the project's per-suite-binary convention). Includes only `NtpSidecarWriter.cpp` plus existing `test/mocks/` (`MockFS`, `MockTime`).

| Test | What it verifies |
|---|---|
| `test_isDatalogEdf_matches_session_files` | `/DATALOG/20260507/<file>_PLD.edf` → true; same for BRP/EVE/SAD/SA2/CSL |
| `test_isDatalogEdf_rejects_root_str` | `/STR.edf` → false |
| `test_isDatalogEdf_rejects_state_files` | `/DATALOG/20260507/.upload_state.v2.smb` → false |
| `test_isDatalogEdf_rejects_non_8digit_folder` | `/DATALOG/foo/bar.edf` → false |
| `test_isDatalogEdf_case_insensitive` | `.EDF` and `.edf` both match |
| `test_parseEdfHeader_valid` | Mock 256-byte header → struct populated correctly |
| `test_parseEdfHeader_short_read` | <256 bytes → false |
| `test_parseEdfHeader_bad_date_format` | Corrupt offsets 168–183 → false |
| `test_parseEdfHeader_format_iso_naive` | "04.05.26"+"22.29.52" → `"2026-05-04T22:29:52"` |
| `test_parseEdfHeader_duration_calc` | num_records="100", record_dur="2.5" → 250 sec |
| `test_isNtpSynced_threshold` | epoch < 2024-01-01 → false; epoch > 2024-01-01 → true |
| `test_captureWitness_skips_when_ntp_unsynced` | MockTime returns 0 → `{valid=false, NTP_UNSYNCED}` |
| `test_captureWitness_skips_when_header_parse_fails` | MockFS 100-byte file → `{valid=false, EDF_PARSE_FAILED}` |
| `test_captureWitness_full_path` | MockFS gives 256-byte EDF + MockTime synced → all fields populated |
| `test_serializeJson_includes_all_fields` | Parse JSON, assert all 7 keys present, schema_version==1 |
| `test_serializeJson_iso_utc_format` | `ntp_observed_at` ends `Z`; `fat_mtime` ends `Z`; `edf_header_start` is naive |
| `test_serializeJson_buffer_bounded` | Worst-case payload fits in 512 bytes |

A small inline `MockSMBUploader` (or function-pointer test double) verifies `uploadRawBuffer` is called with the expected remote path (`<edf>.ntp.json`) and JSON-shaped payload.

### Integration / end-to-end (manual, post-flash)

1. Flash to F25 ESP32, run an upload pass against the live AirSense 11.
2. Verify `/mnt/unraid/Misc/DATALOG/<yyyymmdd>/<file>_PLD.edf.ntp.json` exists alongside the EDF.
3. Hand-validate JSON: `jq` it, confirm fields present, sane timestamps.
4. Trigger Sleep ingest re-run. Confirm `Sessions.ClockOffsetSec` populated for that night (~-257s expected — known +4-min AS11 skew).

### What does NOT change

- Existing tests `test_upload_state_manager`, `test_oxyii_sync`, etc. unchanged.
- `SMBUploader::upload` signature and behavior unchanged.
- Cloud / SleepHQ upload path unchanged.
- File-naming, change-detection, and state-manager behavior all unchanged.

## Surface size

Estimated:

- `include/NtpSidecarWriter.h` — ~50 lines
- `src/NtpSidecarWriter.cpp` — ~200 lines (header parse, JSON serialize, capture, write)
- `src/FileUploader.cpp` — +~10 lines, all in `uploadSingleFileSmb`
- `test/test_ntp_sidecar/test_ntp_sidecar.cpp` — ~250 lines

No changes to public-facing config or web UI.

## Rollout

Once merged + flashed:

1. The next upload pass writes sidecars for all NEW EDFs.
2. Sleep ingest's existing self-heal hook will populate `ClockOffsetSec` for any session that overlaps a sidecar-bearing EDF the next time `Program.cs` boots (or on next ingest cycle).
3. Backfill of existing EDFs in `/mnt/unraid/Misc` is out of scope. Their `ClockOffsetSec` stays NULL — acceptable, since the project memory already notes the AS11 is ~4 min ahead and that's tracked under epic #174.
