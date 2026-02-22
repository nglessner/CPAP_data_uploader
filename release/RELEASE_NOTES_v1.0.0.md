## 🎉 First Stable Release

This is the first stable release of the CPAP Data Uploader firmware. The v1.0 milestone represents a mature, production-ready system with comprehensive documentation, and a fully validated configuration system.

---

## 📋 What's New in v1.0.0

### Smart Upload System
- **Smart Mode:** Automatic uploads when CPAP is idle (24/7 monitoring)
- **Scheduled Mode:** Uploads only during configured time windows
- **Bus Activity Detection:** Hardware-based SD card activity monitoring
- **Exclusive Access Control:** Prevents conflicts with CPAP machine

### Dual Upload Support
- **SMB/Network Shares:** Upload to Windows shares, NAS, or Samba servers
- **SleepHQ Cloud:** Direct cloud integration with OAuth2 authentication
- **Simultaneous Upload:** Send data to both destinations at once

### Security & Reliability
- **State Persistence:** Upload progress tracked across reboots
- **Retry Logic:** Automatic retry with exponential backoff

### New improved web interface
- **Live Dashboard:** Real-time upload status and progress
- **Configuration Editor:** Edit config.txt directly from browser
- **Log Viewer:** Browse and search device logs
- **OTA Updates:** Firmware updates over WiFi
- **SD Activity Monitor:** Real-time bus activity visualization

### Power Management
- **Configurable CPU Speed:** 80/160/240 MHz options
- **WiFi Power Control:** Adjustable transmit power and power saving modes
- **Optimized for 24/7 Operation:** Low power consumption during idle periods

---

## 📦 What's Included

### Firmware Binaries
- `firmware.bin` - Main application firmware
- `bootloader.bin` - ESP32 bootloader
- `partitions.bin` - Partition table

### Configuration Examples
- `config.txt.example.simple` - Minimal configuration (6-10 lines)
- `config.txt.example.smb` - SMB/network share setup
- `config.txt.example.sleephq` - SleepHQ cloud setup
- `config.txt.example.both` - Dual upload (SMB + Cloud)

### Tools & Scripts
- `upload_firmware.py` - Firmware upload script (cross-platform)
- `upload_firmware.bat` - Windows firmware upload script
- `upload_firmware.sh` - Linux/Mac firmware upload script

---

## 🔄 Migration from v0.7.x or Earlier

If you're upgrading from v0.7.x (JSON configuration), you must create a new configuration file.

### Migration Steps

1. **Backup your current setup** (optional but recommended)
   - Note your current settings from `config.json`

2. **Create new `config.txt`**
   - Use one of the provided examples as a template (`config.txt.example*`)
   - Copy your settings manually using KEY = VALUE format
   - Delete or rename your old `config.json`

3. **Flash v1.0.0 firmware** using the included upload scripts

4. **Deploy and verify**
   - Copy the new `config.txt` to the root of your SD card
   - Insert SD card and power on
   - Verify operation at `http://cpap.local`

### Key Name Changes

Some keys have been renamed for consistency:
- `WIFI_PASS` → `WIFI_PASSWORD`
- `ENDPOINT_PASS` → `ENDPOINT_PASSWORD`

### Deprecated Keys

The following keys are no longer supported:
- `BOOT_DELAY_SECONDS` - Hardcoded to 15 seconds
- `SCHEDULE` - Replaced by `UPLOAD_MODE`, `UPLOAD_START_HOUR`, `UPLOAD_END_HOUR`
- Legacy timing parameters: `UPLOAD_HOUR`, `SESSION_DURATION_SECONDS`, `SD_RELEASE_INTERVAL_SECONDS`, `SD_RELEASE_WAIT_MS`, `UPLOAD_INTERVAL_MINUTES`

---

## 🐛 Bug Fixes

### SD card error
- Fixed SD card error which was caused by CPAP machine attempting to access the SD card at the same time as the firmware.
-  
---

## 📖 Configuration Reference

### Minimal Configuration (6 lines)

```ini
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword
ENDPOINT_TYPE = SMB
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
```

### All Available Parameters

See `docs/CONFIG_REFERENCE.md` for complete documentation of all configuration parameters, including:

- Network settings (WiFi, hostname)
- Upload destinations (SMB, Cloud, WebDAV)
- Upload scheduling (smart vs scheduled mode)
- Upload timing (inactivity detection, session duration, cooldown)
- Power management (CPU speed, WiFi power)
- Security options (credential storage)
- Debugging options (logging, verbose mode)

---

## 🔧 Technical Details

### System Requirements
- **Hardware:** SD WIFI PRO (ESP32-PICO-D4)
- **SD Card:** FAT32 formatted, 2GB-32GB recommended
- **WiFi:** 2.4GHz network (5GHz not supported by ESP32)
- **Python:** 3.6+ for migration tool and firmware upload scripts

### Supported CPAP Machines
- ResMed AirSense 9
- ResMed AirSense 10
- ResMed AirSense 11
- Other CPAP machines with SD card data logging (may require testing)

### Configuration File Format
- **File name:** `config.txt` (must be in SD card root directory)
- **Format:** KEY = VALUE (one setting per line)
- **Comments:** Lines starting with `#` are ignored
- **Case insensitive:** Keys are case-insensitive (WIFI_SSID = wifi_ssid)
- **Quotes optional:** Values can be quoted or unquoted

### Credential Security
- Passwords automatically migrated to encrypted ESP32 NVS flash on first boot
- Original values in `config.txt` replaced with `***STORED_IN_FLASH***`
- Credentials persist across firmware updates
- Can be reset by writing new plain text values to `config.txt`

---

## 🚨 Known Issues

### SD Card Errors on Some CPAP Models
Some CPAP machines (particularly AirSense 11) may show SD card errors when using Smart Mode.

**Solution:** Use Scheduled Mode instead:
```ini
UPLOAD_MODE = scheduled
UPLOAD_START_HOUR = 9
UPLOAD_END_HOUR = 23
```

This ensures the device only accesses the SD card during hours when you're not using your CPAP.

### mDNS Not Working on Some Networks
The `http://cpap.local` hostname may not work on all networks (particularly corporate or guest networks).

**Solution:** Use the device's IP address instead. Find it in your router's DHCP client list or check the serial console output during boot. this requires to use the uploader card and connect to the serial port using the monitor.sh (linux and mac) or monitor.bat (windows) scripts. examples of use in the release readme file and the release folder.

---

## 🙏 Acknowledgments

This project builds on the work of many open source contributors and libraries:

- **ESP32 Arduino Core** - Espressif Systems
- **libsmb2** - Ronnie Sahlberg
- **ArduinoJson** - Benoît Blanchon (used for web API responses)
- **Unity Test Framework** - ThrowTheSwitch.org

Special thanks to the CPAP user community for testing, feedback, and feature requests.

---

## 📄 License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0).

See the LICENSE file for full license text.

---

## 🔗 Resources

- **Project Repository:** [GitHub](https://github.com/amanuense/cpap-data-uploader)
- **Hardware:** [SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro)
- **SleepHQ:** [https://sleephq.com](https://sleephq.com)
- **Support:** Open an issue on GitHub

---

## 🎯 What's Next?

v1.0 represents a stable foundation. Future development will focus on:
- Improved CPAP machine compatibility

Stay tuned for updates!
