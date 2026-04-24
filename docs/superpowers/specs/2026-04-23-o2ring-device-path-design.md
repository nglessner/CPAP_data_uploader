# O2Ring SMB Path — Per-Device Subdirectory

**Date:** 2026-04-23
**Branch:** feature/o2ring-ble-sync

## Problem

The server-side oximetry staging pipeline requires uploaded `.vld` files to live under a per-device subdirectory:

```
oximetry/raw/<device>/<filename>.vld
```

The current firmware writes directly to the base path:

```
/oximetry/raw/<filename>.vld
```

(from `src/O2RingSync.cpp:108-110`, with default `o2ringPath = "oximetry/raw"` in `src/Config.cpp:42`).

Every uploaded file is therefore rejected by the staging pipeline. The server does not care what the `<device>` segment *is* semantically — it only needs uniqueness per uploading device so streams from different sources don't collide.

## Goals

1. Fix the upload path so files land at `/oximetry/raw/<device>/<filename>.vld`.
2. Make the `<device>` segment resolve automatically so unconfigured units work out-of-the-box.
3. Allow a human-readable override via `config.txt` for users who manage multiple uploaders.

## Non-Goals

- No runtime path changes. The `<device>` segment is resolved once at config load and cached.
- No behavioral change for SMB uploads outside of O2Ring sync.
- No change to the OTA partition scheme or any other compile-time flag.
- Not covering multi-O2Ring-per-uploader cases. One uploader = one `<device>` segment.

## Design

### Config surface

Add one new optional key to `config.txt`:

| Key           | Default       | Description                                                                                              |
|---------------|---------------|----------------------------------------------------------------------------------------------------------|
| `DEVICE_NAME` | *(empty)*     | Human-readable identifier for this uploader. Used as the `<device>` segment in the O2Ring SMB path. If empty, the uploader's WiFi MAC (colons stripped, lowercased) is used instead. |

`O2RING_PATH` default stays `"oximetry/raw"` — it is the *base* staging path, unchanged. The device segment is composed separately at the upload call site.

The key is named `DEVICE_NAME` rather than `O2RING_DEVICE_NAME` so it can also serve as a general-purpose human label (logs, web UI, future uses).

### Resolution rule

`Config` exposes a new accessor:

```cpp
const String& getDeviceSegment() const;
```

Value is computed once during `Config::load()` and cached. Rule:

1. If `DEVICE_NAME` is set and its sanitized form is non-empty → use the sanitized form.
2. Otherwise → WiFi MAC (`WiFi.macAddress()`), colons stripped, lowercased, passed through the same sanitizer (defense-in-depth; MAC is already safe).

The computed segment is stored in a new `String deviceSegment` member on `Config`.

### Sanitizer

A single static helper (exposed for unit testing):

```cpp
static String sanitizeDeviceSegment(const String& raw);
```

Rules, applied in order:

1. Iterate raw characters. Keep `[a-zA-Z0-9_-]`; replace any other character (including `/`, `\`, `:`, whitespace, non-ASCII) with `-`.
2. Collapse any run of consecutive `-` to a single `-`.
3. Trim leading and trailing `-`.
4. Truncate to 32 characters (trim again after truncation in case truncation left a trailing `-`).
5. If the result is empty, return empty string — caller treats this as "no name set" and falls back.

Characters allowed cover SMB/CIFS safe path semantics (no colons — Windows reserves `:` for alternate data streams; no slashes — would alter path structure).

### Path composition

At `src/O2RingSync.cpp:108`, the existing two lines are replaced:

```cpp
String dir = "/" + config->getO2RingPath() + "/" + config->getDeviceSegment();
smb.createDirectory(dir);
String remotePath = dir + "/" + filename;
bool uploaded = smb.uploadRawBuffer(remotePath, fileData.data(), fileData.size());
```

`SMBUploader::createDirectory` already walks and creates parent directories recursively (`src/SMBUploader.cpp:353-361`), so passing the full nested path in one call handles the hierarchy. No helper is needed.

Because `getDeviceSegment()` is never empty (fallback guarantees at least a MAC-derived value), the upload path is always well-formed.

### WiFi MAC availability

`WiFi.macAddress()` is available after `WiFi.begin()` or on any ESP32 at any time after boot — the MAC is burned into eFuses and does not require network connectivity. We read it during `Config::load()`, which runs after WiFi init in `main.cpp`. If `Config::load()` ever runs before WiFi init (e.g., during refactoring), `WiFi.macAddress()` still returns the hardware MAC; the ordering is not load-bearing.

For the unit-test build (`UNIT_TEST`), `WiFi.macAddress()` is stubbed to return a fixed value from the Arduino mock so tests are deterministic.

## Testing

New native suite `test/test_device_name/` with its own `main()` (per project convention — one main per test directory).

Cases:

**Sanitizer** (`sanitizeDeviceSegment`)
- Valid identifier passes through unchanged: `"neil-bedroom"` → `"neil-bedroom"`.
- Spaces replaced: `"my device"` → `"my-device"`.
- Collision chars replaced: `"a/b\\c:d"` → `"a-b-c-d"`.
- Consecutive invalid collapse: `"a   b"` → `"a-b"`.
- Leading/trailing trimmed: `"---foo---"` → `"foo"`.
- Empty input → empty output.
- All-invalid input → empty output: `"///"` → `""`.
- 32-char cap: 40 `a` chars → 32 `a` chars.
- Cap + trailing `-` trim: `"aaaa...aaaa-"` truncated to exactly 32 and then trimmed if the 32nd char is `-`.

**Resolution** (`Config::load` behavior under mocked `config.txt` input and mocked `WiFi.macAddress`)
- `DEVICE_NAME` unset → returns sanitized MAC: `"ac0bfb6fa194"`.
- `DEVICE_NAME=""` (empty) → returns sanitized MAC.
- `DEVICE_NAME="   "` (whitespace only) → sanitizer returns empty → fallback to MAC.
- `DEVICE_NAME="home-upload"` → returns `"home-upload"`.
- `DEVICE_NAME="home upload!"` → returns `"home-upload"`.

Path composition does not need a dedicated test — it is two-line string concatenation and the sanitizer + resolver tests cover all the variable inputs.

## Docs

- Add `DEVICE_NAME` row to `docs/CONFIG_REFERENCE.md` in the appropriate section (general/system, not WiFi or endpoint).
- Update the O2Ring section of `docs/DEVELOPMENT.md` (or wherever the upload path is described) to note the `<base>/<device>/<file>` layout and point at `DEVICE_NAME`.
- Update `CLAUDE.md` (`CPAP_data_uploader/CLAUDE.md`) BLE/O2Ring layer paragraph to mention the per-device subdir invariant, so future sessions don't regress it.

## Rollout

- Single PR to `feature/o2ring-ble-sync` (current branch), incorporated into the eventual merge to `main`/upstream.
- Existing `config.txt` files continue to work unchanged — the fallback keeps previously-deployed uploaders functional after firmware update.
- Server-side, previously-failed uploads do not auto-retry; the path is a forward-only fix. If re-upload of stale files is needed, clear the NVS dedup set or delete the affected filenames from it.

## Risks

- **Fallback MAC collision** — two uploaders with manufacturer-assigned MACs will never collide; the risk is zero. Documented here for completeness.
- **DEVICE_NAME change mid-life** — if the user changes `DEVICE_NAME` later, the device starts writing to a new folder. Prior files remain under the old folder. Acceptable — the server treats each folder as opaque; this is equivalent to adding a new uploader.
- **Long MAC sanitization output** — stripped MAC is 12 chars, well under the 32-char cap, so truncation doesn't interact with the fallback branch.
