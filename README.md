# Deco Optimizer

Runs the TP-Link Deco mesh **network optimization** (scan, then optimize if the Deco says it will help) from an
ESP32-C3 with a 1.28" round touch display (Guition ESP32-2424S012: GC9A01 + CST816S), on demand or on a daily schedule.
Status and controls are on the display, on a small web page, and in Home Assistant over MQTT.

The Deco mesh has no local API for this feature. The Deco app does it over an SSH tunnel into the master Deco, speaking a
binary protocol ("TMP") to a service on the Deco's loopback interface; this project reproduces that. Protocol notes
are kept separately from this repo.

## Features
- Scan / optimize via SSH direct-tcpip to `127.0.0.1:20002` on the Deco (libssh2 on mbedTLS; the Deco's 512-bit RSA host key
  is pinned on first use).
- Touch UI: status page, swipeable history, Run now, report-only / optimise switch.
- Daily schedule (SNTP, POSIX timezone in `main/config.h`). A report-only scan always runs once after boot, and a reboot never optimises.
- Home Assistant MQTT discovery (state, last/next scan, optimise switch, run button), with availability.
- Wi-Fi tuned for a multi-node mesh: joins the strongest node, pings the gateway before each run and moves off a node
  that is not passing traffic.
- First-run setup: the device starts its own access point (QR code on screen) and a captive portal asks for the Wi-Fi network and a
  web admin password. Everything else is set on the web page. Holding BOOT for 5 s while running re-enters setup.

## Security notes
- All settings and secrets (Wi-Fi, TP-Link login, MQTT login, admin password, pinned host key) are stored in one blob in an
  **encrypted NVS** partition. The key is derived from an HMAC key generated inside the chip and burned to eFuse BLOCK_KEY5
  (`HMAC_UP`) on first boot. **This is irreversible** and uses one of the six key blocks. Secure boot and flash encryption are not enabled,
  so someone who can reflash the chip can still ask it to decrypt; the protection is against reading secrets from a flash dump.
- The web page is **plain HTTP** with Basic auth (user `admin`). Use it on a trusted network only. Secret fields are write-only.
- The TP-Link cloud account password is what the Deco accepts for SSH, so treat the device accordingly.
- "Erase all settings" on the web page wipes everything and returns the device to setup mode.

## Build
ESP-IDF v5.3.x. Chip-specific defaults live in `sdkconfig.defaults.<target>`:

    . $IDF_PATH/export.sh
    idf.py set-target esp32c3        # or esp32s3, esp32c6, ...
    idf.py build flash

`sdkconfig.defaults*` apply only when `sdkconfig` is first generated, so delete `sdkconfig` after changing them.
Options are under `idf.py menuconfig` -> "Deco Optimizer". After flashing the ESP32-2424S012, power-cycle it if the new firmware does not start.

### Headless build (no screen)
Enable **Headless build** (`CONFIG_DECO_HEADLESS`) and set **Setup access point password** (`CONFIG_DECO_SETUP_AP_PASSWORD`, 8-63 characters;
the build fails without it, because there is no screen to show a random one on). The display, touch input and LVGL are replaced by
stubs, and status and control are on the web page, over MQTT/Home Assistant and in the serial log. Headless saves about 330 KB of flash and, measured on an ESP32-C3,
roughly 66 KB of free heap (186 KB free before an SSH session instead of 120 KB; the low point during the handshake is 96 KB instead of 29 KB).
For example, with an overlay file:

    printf 'CONFIG_DECO_HEADLESS=y\nCONFIG_DECO_SETUP_AP_PASSWORD="choose-something"\n' > headless.defaults
    idf.py -B build-headless -DIDF_TARGET=esp32c3 -DSDKCONFIG=build-headless/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;headless.defaults" build

The display and touch libraries are still downloaded and compiled in a headless build (ESP-IDF 5.3's component manager cannot make dependencies depend on a project option), but nothing links them.
Set **Setup-reset button GPIO** (`CONFIG_DECO_RESET_BUTTON_GPIO`, `-1` to disable) to match your board's button.

### Chip support
| Chip | Status | Notes |
|---|---|---|
| ESP32-C3 | Tested on hardware, with screen and headless | The reference board. |
| ESP32-S3, ESP32-C6 | Headless build compiles; not run | Have the HMAC peripheral, so the encrypted-NVS design applies unchanged. The screen code is specific to the GC9A01/CST816S board and its pins (`display.c`). |
| ESP32-S2 | Not built | Has HMAC; no USB-serial-JTAG, so the console is UART. 2.4 GHz Wi-Fi, similar RAM to the C3. |
| ESP32 (classic) | Not built | **No HMAC peripheral**: the NVS encryption scheme used here is unavailable, so secrets would be stored unencrypted unless you set up flash encryption and the `nvs_keys` scheme. Plenty of RAM. |
| ESP32-C2 | Not recommended | Probably too little RAM for the SSH handshake (about 78 KB transient) next to Wi-Fi, and no HMAC. |
| ESP32-C5 | Not built | Needs a newer ESP-IDF than 5.3. |
| ESP32-H2, ESP32-P4 | No | No Wi-Fi. |

## Layout
`main/deco.c` (SSH + TMP client), `tmp_proto.c` (framing), `wifi.c` (mesh-aware station), `settings.c` (encrypted config),
`provision.c` (setup AP + portal), `web.c` + `web/index.html`, `mqtt.c`, `ui.c`, `display.c`, `history.c`.

## Known limitations
- Free RAM is tight with the screen: the SSH handshake briefly needs about 78 KB of the roughly 120 KB that is free when idle, which is why the
  web server is HTTP only. A headless build has roughly 66 KB more.
- The Deco's own scan briefly drops Wi-Fi clients, including this device, so Home Assistant entities flicker "unavailable" during scans.
- No licence has been chosen yet.
