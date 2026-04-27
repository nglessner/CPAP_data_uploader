# pico32-ota build fix: gate O2Ring sources behind `ENABLE_O2RING_SYNC`

**Date:** 2026-04-26
**Closes:** [nglessner/CPAP_data_uploader#8](https://github.com/nglessner/CPAP_data_uploader/issues/8)

## Problem

`pio run -e pico32-ota` fails with:

```
fatal error: NimBLEDevice.h: No such file or directory
```

Root cause: `src/Esp32BleClient.cpp` is gated only by `#ifndef UNIT_TEST` — not by `#ifdef ENABLE_O2RING_SYNC` — so it gets compiled in `pico32-ota`'s production build. That env's `lib_deps` (platformio.ini line 133) does not include `h2zero/NimBLE-Arduino`, so the header chain breaks at preprocess time.

Three other O2Ring source files (`O2RingSync.cpp`, `O2RingState.cpp`, `O2RingStatus.cpp`) are similarly ungated — they compile under any environment that touches the project, regardless of the feature flag. Today they happen not to break the build because they don't pull NimBLE headers, but they're ticking time bombs: any future dependency they grow becomes a hard build failure on `pico32-ota`.

The caller side (`main.cpp`, `CpapWebServer.cpp`) gates references to these files correctly with `#ifdef ENABLE_O2RING_SYNC`. Only the implementation files leak.

## Goal

The feature flag should genuinely disable all O2Ring code, including its compile-time dependencies. After this change:

- `pio run -e pico32` (flag on) — full O2Ring build, behavior unchanged.
- `pio run -e pico32-ota` (flag off) — succeeds. O2Ring TUs compile to empty.
- `pio test -e native` (UNIT_TEST + flag on after this change) — all 176 tests still pass.

Out of scope:
- Adding NimBLE to `pico32-ota`'s `lib_deps`. The flag should disable the dep too.
- Refactoring existing `#ifdef ENABLE_O2RING_SYNC` blocks in `main.cpp` / `CpapWebServer.cpp`. Already correct.
- Documentation churn. The `platformio.ini:127` comment ("O2Ring is incompatible with pico32-ota's 1.5MB partition") still accurately states the runtime limitation; this fix just makes the build correctly enforce it.

## Design

### Source file guards

`src/Esp32BleClient.cpp` — replace:

```cpp
#ifndef UNIT_TEST

// ... full file ...

#endif // UNIT_TEST
```

with:

```cpp
#if defined(ENABLE_O2RING_SYNC) && !defined(UNIT_TEST)

// ... full file ...

#endif // ENABLE_O2RING_SYNC && !UNIT_TEST
```

This file's only purpose is the real-hardware BLE backend for O2Ring; it should compile only when the feature is on AND we're producing a firmware (not running unit tests, where the mock substitutes).

`src/O2RingSync.cpp`, `src/O2RingState.cpp`, `src/O2RingStatus.cpp` — wrap each entire file in:

```cpp
#ifdef ENABLE_O2RING_SYNC

// ... full file ...

#endif // ENABLE_O2RING_SYNC
```

These files DO compile in unit tests (test files include them directly via `#include "../../src/O2RingState.cpp"`), so they don't need the additional `!UNIT_TEST` guard — the native env will define `ENABLE_O2RING_SYNC` after this change.

### Native env build flag

`platformio.ini` `[env:native]` — add `-DENABLE_O2RING_SYNC` to `build_flags`. Current state:

```ini
[env:native]
platform = native
build_flags = 
    -DUNIT_TEST
```

becomes:

```ini
[env:native]
platform = native
build_flags = 
    -DUNIT_TEST
    -DENABLE_O2RING_SYNC
```

Semantically honest: the test suite exercises O2Ring code paths (`test_o2ring`, `test_o2ring_sync`, `test_o2ring_state`, `test_o2ring_status`). Making the flag explicit captures that intent and lets the source files use a single uniform guard.

### Header files

Headers (`include/Esp32BleClient.h`, `include/O2RingSync.h`, etc.) are NOT modified. Today they're only included from sites that already check `#ifdef ENABLE_O2RING_SYNC` themselves — including a never-instantiated header is a no-op. Adding guards to headers would require synchronized changes at every include site for no benefit.

`include/Esp32BleClient.h` already has its own `#ifndef UNIT_TEST` guard for the class declaration. That stays as-is.

## Compatibility

- Existing pico32 deployments: no behavior change. The flag is on, the code is included, runtime identical.
- Existing pico32-ota deployments: build now succeeds where it was failing — net positive.
- Future code touching O2Ring: must respect the flag at the implementation file's top. The pattern is now uniform across all four O2Ring `.cpp` files, so the convention is easy to follow.

## Verification

This is a build-system fix; the test is whether each environment compiles correctly:

1. `pio test -e native` — 176/176 pass (the new build flag makes the code compile the same way it did, just with explicit gating).
2. `pio run -e pico32` — succeeds, no size regression (same code as before).
3. `pio run -e pico32-ota` — succeeds where it was failing. Reports realistic flash usage for the OTA partition; this is a useful baseline for whether anything else is creeping toward the limit.

## Acceptance

- All four O2Ring source files are wrapped in `#ifdef ENABLE_O2RING_SYNC` (or the combined guard for `Esp32BleClient.cpp`).
- `[env:native]` in `platformio.ini` includes `-DENABLE_O2RING_SYNC` in `build_flags`.
- `pio test -e native` passes 176/176.
- `pio run -e pico32` succeeds.
- `pio run -e pico32-ota` succeeds (regression fixed).
