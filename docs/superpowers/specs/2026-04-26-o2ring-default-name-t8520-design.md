# O2Ring default device name: `O2Ring` → `T8520`

**Date:** 2026-04-26
**Closes:** [nglessner/CPAP_data_uploader#4](https://github.com/nglessner/CPAP_data_uploader/issues/4)

## Problem

`Config.cpp:47` sets the default `O2RING_DEVICE_NAME` to `"O2Ring"`, and `docs/CONFIG_REFERENCE.md` documents that as the default. Real Wellue O2Ring-S firmware advertises with the name `T8520_<last4mac>` (Viatom internal model code), so the default never matches a live device.

Verified live device name: `T8520_e85a` at MAC `8C:85:80:BD:E8:5A` (BlueZ scan, 2026-04-25). This is the first thing every new user hits when enabling `-DENABLE_O2RING_SYNC`.

## Goal

Change the compiled-in default to `"T8520"` so out-of-the-box scans match modern Viatom rings. Document the legacy override path so users with older firmware advertising as `O2Ring*` know to set `O2RING_DEVICE_NAME = O2Ring` in `config.txt`.

Out of scope:
- Multi-prefix matching at scan time. The `Esp32BleClient::connect()` loop uses `String::startsWith(namePrefix)` against a single prefix; supporting multiple prefixes simultaneously would require interface changes for a use case (legacy `O2Ring*` rings) the override knob already covers.
- Changing the prefix argument's runtime semantics. Single value, single match, override via config key.

## Design

### Compiled default

`src/Config.cpp:47` — flip the constructor initializer:

```cpp
o2ringDeviceName("T8520"),    // Default: match Viatom O2Ring-S advertised name prefix
```

The comment also updates from "Wellue O2Ring" to "Viatom O2Ring-S" to reflect the actual advertising firmware identity.

### Test update

`test/test_config/test_config.cpp:924` — `test_o2ring_defaults` flips its assertion:

```cpp
TEST_ASSERT_EQUAL_STRING("T8520", config.getO2RingDeviceName().c_str());
```

`test_o2ring_config_load` (the test that exercises explicit override via `O2RING_DEVICE_NAME = ...` in config.txt) is unaffected — it asserts whatever value the test's config string sets, not the default.

### User-facing documentation

`docs/CONFIG_REFERENCE.md` — three edits within section 9 ("O2Ring BLE Sync (optional)"):

1. Table row: `| O2RING_DEVICE_NAME | T8520 | BLE advertised name prefix to scan for |`
2. Example block: `O2RING_DEVICE_NAME = T8520`
3. Add a short paragraph below the example block explaining the legacy override:

```
The default `T8520` matches modern Viatom O2Ring-S firmware (advertised as
`T8520_<last4-mac>`). Older Viatom rings sometimes advertise as `O2Ring`
or another vendor-specific prefix; if the firmware never finds your ring
with the default, override via `O2RING_DEVICE_NAME = <prefix>` after
confirming the actual advertised name with a BLE scanner (e.g.
`bluetoothctl scan on`, the nRF Connect mobile app, etc.).
```

### Tests left unchanged

- The 5 mock JSON strings in `test_o2ring_sync.cpp` carrying `"Model":"O2Ring"` represent the ring's INFO response model field, not the BLE advertised name, and stay as-is.
- The explicit `O2RING_DEVICE_NAME = O2Ring` line in `test_o2ring_sync.cpp:59`'s test config exercises the explicit-override-via-config-key code path; the `MockBleClient::connect()` ignores the prefix argument, so the value here doesn't affect test outcomes. Leaving as-is.

## Compatibility

- Existing users with `O2RING_DEVICE_NAME = O2Ring` (or any other explicit value) in their `config.txt` are unaffected — the default only fires when the key is absent or commented out.
- Existing users relying on the default and an old-firmware `O2Ring*`-advertising ring will need to set `O2RING_DEVICE_NAME = O2Ring` after upgrading firmware. The new doc paragraph documents this path.
- No NVS, persistence, or wire-format changes.

## Acceptance

- `Config::Config()` initializes `o2ringDeviceName` to `"T8520"`.
- `test_o2ring_defaults` passes asserting the new default.
- `docs/CONFIG_REFERENCE.md` shows `T8520` in both the table row and example, plus the legacy-override paragraph.
- `pio test -e native -f test_config` passes.
- `pio run -e pico32` builds without size regression.
