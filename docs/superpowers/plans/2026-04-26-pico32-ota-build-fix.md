# pico32-ota Build Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `pio run -e pico32-ota` succeed by gating all four O2Ring source files behind `#ifdef ENABLE_O2RING_SYNC` so the env's missing NimBLE dependency doesn't break compilation.

**Architecture:** Wrap each `.cpp` in a feature-flag guard, and add `-DENABLE_O2RING_SYNC` to the native env so unit tests (which include the `.cpp` bodies directly) still compile. Headers and caller sites are already correct and stay untouched.

**Tech Stack:** PlatformIO multi-env builds, ESP-IDF / Arduino framework on pico32 / pico32-ota, Unity unit tests on native.

**Spec:** `docs/superpowers/specs/2026-04-26-pico32-ota-o2ring-source-gating-design.md`

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `platformio.ini` | Modify | Add `-DENABLE_O2RING_SYNC` to `[env:native]` build_flags |
| `src/O2RingState.cpp` | Modify | Wrap entire file in `#ifdef ENABLE_O2RING_SYNC` |
| `src/O2RingStatus.cpp` | Modify | Wrap entire file in `#ifdef ENABLE_O2RING_SYNC` |
| `src/O2RingSync.cpp` | Modify | Wrap entire file in `#ifdef ENABLE_O2RING_SYNC` |
| `src/Esp32BleClient.cpp` | Modify | Replace `#ifndef UNIT_TEST` with `#if defined(ENABLE_O2RING_SYNC) && !defined(UNIT_TEST)` |

No new files, no header changes, no caller-site changes.

---

## Task 1: Add `-DENABLE_O2RING_SYNC` to the native env

This must land before any source guards. Without it, Task 2's guards would make the native test bodies compile to empty TUs and the linker would fail to find `O2RingState`, `O2RingStatus`, etc.

This task is a build-flag addition only; no source files change. Native tests should still pass 176/176 because the flag has no effect when no source consults it yet.

**Files:**
- Modify: `platformio.ini` `[env:native]` block

- [ ] **Step 1: Add the build flag**

In `platformio.ini`, locate the `[env:native]` block:

```ini
[env:native]
platform = native
build_flags = 
    -DUNIT_TEST
```

Replace with:

```ini
[env:native]
platform = native
build_flags = 
    -DUNIT_TEST
    -DENABLE_O2RING_SYNC
```

- [ ] **Step 2: Verify native tests still pass**

Run: `source venv/bin/activate && pio test -e native`

Expected: 176/176 pass. The flag is now defined but no source file consults it yet, so behavior is unchanged.

- [ ] **Step 3: Verify pico32 build still passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: succeeds. This env already had the flag.

- [ ] **Step 4: Commit**

```bash
git add platformio.ini
git commit -m "build(native): define ENABLE_O2RING_SYNC for unit tests

Test files include O2RingSync.cpp / O2RingState.cpp / O2RingStatus.cpp
directly. Subsequent commits add #ifdef ENABLE_O2RING_SYNC guards to
those source files so the pico32-ota production build (which doesn't
ship O2Ring) compiles cleanly. The native env defines the flag because
the tests do exercise the gated code."
```

---

## Task 2: Gate `O2RingState.cpp`, `O2RingStatus.cpp`, `O2RingSync.cpp`

Wrap each in `#ifdef ENABLE_O2RING_SYNC` ... `#endif`. These three don't interact with NimBLE directly, so they don't break the pico32-ota build today — but they're still semantic leaks (compiled in production builds where the feature is off). Closing the leak now keeps the convention uniform across all four O2Ring `.cpp` files.

This task by itself does NOT fix `pico32-ota` — `Esp32BleClient.cpp` is still ungated and still pulls NimBLE. That's Task 3.

**Files:**
- Modify: `src/O2RingState.cpp`
- Modify: `src/O2RingStatus.cpp`
- Modify: `src/O2RingSync.cpp`

- [ ] **Step 1: Wrap `O2RingState.cpp`**

At the top of `src/O2RingState.cpp`, insert before the first `#include`:

```cpp
#ifdef ENABLE_O2RING_SYNC

```

At the very bottom of the file (after the last line of code), append:

```cpp

#endif // ENABLE_O2RING_SYNC
```

- [ ] **Step 2: Wrap `O2RingStatus.cpp`**

At the top of `src/O2RingStatus.cpp`, insert before the first `#include`:

```cpp
#ifdef ENABLE_O2RING_SYNC

```

At the bottom, append:

```cpp

#endif // ENABLE_O2RING_SYNC
```

- [ ] **Step 3: Wrap `O2RingSync.cpp`**

At the top of `src/O2RingSync.cpp`, insert before the first `#include`:

```cpp
#ifdef ENABLE_O2RING_SYNC

```

At the bottom, append:

```cpp

#endif // ENABLE_O2RING_SYNC
```

- [ ] **Step 4: Verify native tests still pass**

Run: `source venv/bin/activate && pio test -e native`

Expected: 176/176 pass. The native env defines `ENABLE_O2RING_SYNC` (Task 1), so the source bodies still compile.

- [ ] **Step 5: Verify pico32 build still passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: succeeds. The pico32 env defines the flag, so all three files compile as before.

- [ ] **Step 6: Verify pico32-ota build state**

Run: `source venv/bin/activate && pio run -e pico32-ota`

Expected: still FAILS with the same `NimBLEDevice.h: No such file or directory` from `Esp32BleClient.cpp`. Three of the four files are now correctly gated, but the fourth still leaks. This is the expected intermediate state — Task 3 finishes the job.

If a different error appears (e.g., a previously-hidden compile error in one of the three files we just gated), STOP and report. Don't proceed.

- [ ] **Step 7: Commit**

```bash
git add src/O2RingState.cpp src/O2RingStatus.cpp src/O2RingSync.cpp
git commit -m "build(o2ring): gate orchestrator/state/status sources behind feature flag

Wrap O2RingState.cpp, O2RingStatus.cpp, and O2RingSync.cpp in
#ifdef ENABLE_O2RING_SYNC so they're skipped in production builds where
the feature is off. Esp32BleClient.cpp is the actual NimBLE leak in
pico32-ota; that lands in the next commit. Per #8."
```

---

## Task 3: Gate `Esp32BleClient.cpp`

This is the file that actually breaks the `pico32-ota` build by pulling `<NimBLEDevice.h>`. Combine the existing `#ifndef UNIT_TEST` guard with the feature flag so the file compiles only when both apply.

**Files:**
- Modify: `src/Esp32BleClient.cpp`

- [ ] **Step 1: Replace the existing guard**

At the top of `src/Esp32BleClient.cpp`, replace:

```cpp
#ifndef UNIT_TEST
```

with:

```cpp
#if defined(ENABLE_O2RING_SYNC) && !defined(UNIT_TEST)
```

At the bottom of `src/Esp32BleClient.cpp`, replace the closing line:

```cpp
#endif // UNIT_TEST
```

with:

```cpp
#endif // ENABLE_O2RING_SYNC && !UNIT_TEST
```

(If the bottom guard's comment text is slightly different — e.g., `#endif`, `#endif // !UNIT_TEST`, etc. — match whichever form is there; what matters is that it pairs with the new top guard.)

- [ ] **Step 2: Verify native tests still pass**

Run: `source venv/bin/activate && pio test -e native`

Expected: 176/176 pass. Native still defines UNIT_TEST so the file is excluded (the mock substitutes), unchanged from before.

- [ ] **Step 3: Verify pico32 build still passes**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: succeeds. Both flags are present in this env, so the file compiles as before.

- [ ] **Step 4: Verify pico32-ota build now passes**

Run: `source venv/bin/activate && pio run -e pico32-ota`

Expected: succeeds. `ENABLE_O2RING_SYNC` is not defined in this env, so the file's body is skipped, NimBLE is never referenced, and the lib_deps gap stops mattering. Report flash/RAM percentages from the size summary.

- [ ] **Step 5: Commit**

```bash
git add src/Esp32BleClient.cpp
git commit -m "build(ble): gate Esp32BleClient behind ENABLE_O2RING_SYNC

Combines the existing UNIT_TEST guard with the feature flag so the file
compiles only when O2Ring is enabled AND we're producing a firmware
(not running unit tests, where MockBleClient substitutes). Fixes the
pico32-ota build, which doesn't ship NimBLE in lib_deps. Closes #8."
```

---

## Task 4: Final verification

No code changes. Confirm cumulative state across all three environments.

- [ ] **Step 1: Full native suite**

Run: `source venv/bin/activate && pio test -e native`

Expected: 176/176 pass.

- [ ] **Step 2: pico32 build**

Run: `source venv/bin/activate && pio run -e pico32`

Expected: succeeds. Note flash/RAM lines for any unexpected jumps; the change is preprocessor-only with no runtime effect, so size should be identical to before.

- [ ] **Step 3: pico32-ota build**

Run: `source venv/bin/activate && pio run -e pico32-ota`

Expected: succeeds. This is the regression fix. Note flash/RAM percentages — useful baseline going forward.

- [ ] **Step 4: Confirm clean tree**

Run: `git status`

Expected: working tree clean except for pre-existing items (`.gitignore` modification and untracked plan docs from earlier in the session, unrelated to this work).

- [ ] **Step 5: Review branch's commit log**

Run: `git log --oneline main..HEAD`

Expected: 4 commits (counting the spec commit already on the branch):
1. `docs(build): spec for gating O2Ring sources behind ENABLE_O2RING_SYNC` (already committed)
2. `build(native): define ENABLE_O2RING_SYNC for unit tests`
3. `build(o2ring): gate orchestrator/state/status sources behind feature flag`
4. `build(ble): gate Esp32BleClient behind ENABLE_O2RING_SYNC`

Each commit is independently buildable on at least the envs the plan promises (Task 2's commit leaves pico32-ota broken — that's documented as expected — but native and pico32 stay green throughout).
