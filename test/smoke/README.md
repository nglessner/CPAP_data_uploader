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
