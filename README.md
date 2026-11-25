# LAMAX X3.1 Atlas → WiFi RTSP security camera

Turns the **LAMAX X3.1 Atlas** action camera (Allwinner V3, Sony IMX175) into a
WiFi security camera: after power-on it joins your WiFi and serves an
**H.264 RTSP stream** that any NVR, VLC, ffmpeg, Home Assistant, Frigate … can record.

* Everything runs from the **SD card** (`cam/` folder: programs + `config.txt`).
* The camera needs **one small, one-time change** in its flash: a boot hook in the
  boot partition that starts `cam/autorun.sh` if it exists on the SD card.
  Without that folder (or with `MODE=stock`) the camera works exactly like before.
* A desktop tool (**LamaxTool**, Windows and Linux) backs up the flash, installs the
  hook and prepares SD cards – with a WiFi scan to pick the network – so many
  cameras can be converted quickly.

> ⚠️ This modifies your camera's firmware. A full flash backup is made first and
> the write is verified by reading it back, but you do this at your own risk.
> Only the boot partition is written; the bootloader is never touched, so the
> camera stays recoverable through the Allwinner FEL USB mode.

## Features

| | |
|---|---|
| Video | 1920×1080 or 1280×720, 1–30 fps, H.264 baseline/main/high, hardware encoder (≈13 ms per 1080p frame) |
| RTSP | `rtsp://<ip>:554/stream`, RTP over UDP or TCP (interleaved), Digest + Basic auth, up to 8 clients |
| Network | 2.4 GHz WiFi, WPA2-PSK or open, DHCP or static IP, automatic reconnect |
| Time stamp | date and time burned into the picture (optionally with the camera name), any corner; time from the network (SNTP), POSIX time zone |
| Screen | status page: WiFi name, **live signal strength (dBm)** for finding a good mounting spot, IP, clients, bitrate, battery / DC icon; WiFi scan page; sleeps after 60 s, wakes on any button |
| LEDs | blue + top yellow LED show the state (see below), or off |
| Battery | percentage and charging shown on screen; optional long-life charging (4.10 V ≈ 85–90 %) |
| Robustness | hardware watchdog (reboot if the stream hangs), supervisor restarts every part, boot-loop protection, logs on the SD card |

## Quick start

1. Download LamaxTool from the [Releases](../../releases) page – `LamaxTool.exe` for
   Windows or `LamaxTool` for Linux (x86-64) – or build it, see [docs/BUILD.md](docs/BUILD.md).
   It contains the camera programs and the boot hook.
2. Plug the camera into the computer with a USB data cable and choose **mass storage**
   on the camera when it asks. adb (Android platform-tools) is downloaded from Google
   on first use if it is not found.
   *Linux:* allow USB access once, then re-plug the camera:
   ```bash
   echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="1f3a", MODE="0666"' | sudo tee /etc/udev/rules.d/51-lamax.rules
   sudo udevadm control --reload-rules
   ```
3. In LamaxTool:
   1. **Back up flash** – all 8 partitions, each read twice and compared, saved to
      `backups/<serial>_<date>/` next to the program. Keep this folder.
   2. **Install boot hook** – built from *this* camera's own backup, written, read back and compared.
   The window has three numbered sections: **1 Camera flash** (once per camera),
   **2 Settings** (edit / open / save `config.txt`) and **3 Write to SD card**.
   3. Set up the camera on the **Network / Video / Time / RTSP / Device** tabs – every
      `config.txt` setting is there, with range checks. **Scan WiFi w. camera** lists the
      networks the camera sees (see below), so the name can be picked instead of typed. Settings can be loaded from a `config.txt` file or straight from the
      camera's SD card, saved to a file (e.g. as a template for more cameras) or reset to defaults.
      Then **Write to camera SD card** (or **Write to drive / folder** for a card in a card reader).
      An existing `config.txt` on the card is only replaced when *Overwrite existing config.txt* is ticked.
      The last used settings (passwords included, in plain text) are remembered in
      `LamaxTool_settings.txt` next to the program – or in `%APPDATA%\LamaxTool\` /
      `~/.config/lamaxtool/` if the program's folder is read-only. Delete it to start from the defaults.
4. Reboot the camera from its charger / power supply. After ~10 s the status page appears.
5. Add `rtsp://<camera ip>:554/stream` to your NVR as a *generic RTSP* channel
   (prefer **TCP** transport if the NVR offers it).

Manual installation without the tool: [docs/INSTALL.md](docs/INSTALL.md).

### WiFi scan in LamaxTool

**Scan WiFi w. camera** uses the camera's own radio, so it needs a camera that already
runs this software (boot hook installed, SD card written, camera restarted) and is
connected over USB. The list shows each network's signal in dBm – the most accurate
picture of what the camera will see where it is. For the very first setup simply type
the network name.

## Security notes

* **Put the cameras on a separate guest / IoT WiFi network.** The WiFi password is
  stored in clear text in `cam/config.txt`, and anyone holding the camera can read
  everything on it anyway (adb gives a root shell without a password – that is how
  the stock firmware ships). A guest network keeps a stolen camera from being a
  key to your home network.
* Set `RTSP_USER` / `RTSP_PASSWORD` if other devices share the camera network.
* There is no ONVIF and no web interface; only RTSP (TCP 554, UDP 6970/6971) is served.

## Configuration

All settings are in `cam/config.txt` on the SD card (`KEY=value`, comments with `#`;
Windows line endings are fine). The file is self-documenting; the main keys:

| Key | Default | Meaning |
|---|---|---|
| `MODE` | `camera` | `stock` = leave the original software running |
| `CAMERA_NAME` | `camera1` | shown on screen and in the stream description |
| `WIFI_SSID`, `WIFI_PASSWORD` | | 2.4 GHz network; empty password = open network |
| `IP_MODE` | `dhcp` | `static` uses `IP_ADDRESS`, `IP_NETMASK`, `IP_GATEWAY`, `IP_DNS` |
| `VIDEO_WIDTH`/`HEIGHT`/`FPS`/`BITRATE_KBPS` | 1920/1080/25/4000 | |
| `VIDEO_GOP_SECONDS`, `VIDEO_PROFILE`, `VIDEO_FLIP` | 2 / main / 0 | flip = rotate 180° |
| `RTSP_PORT`, `RTSP_PATH`, `RTSP_USER`, `RTSP_PASSWORD` | 554 / stream | |
| `OSD_TIME`, `OSD_POSITION`, `OSD_NAME` | 1 / top-left / 0 | date and time in the picture, corner, camera name in front of it |
| `TIMEZONE` | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX TZ string (examples in `config.txt`; LamaxTool has a list) |
| `NTP_SERVER` | `pool.ntp.org` | time server; the WiFi router is tried as well. Synced after WiFi connects, then every 6 h |
| `SCREEN_TIMEOUT` | 60 | seconds; 0 = always on |
| `LED` | `on` | `off` = status LEDs off |
| `BATTERY_LONG_LIFE` | 1 | charge only to 4.10 V (≈ 85–90 %) to spare an always-powered battery; 0 = full charge |
| `WATCHDOG` | 1 | reboot if the stream program hangs |
| `LOG` | `sd` | `sd` = `cam/log.txt` (max 2 × 512 KB, rotated while running), `ram` = RAM only, nothing written to the card, `off` |
| `WIFI_DEBUG_LOG` | 0 | verbose `cam/wpa_log.txt` (max 2 × 1 MB) |

## Screen and buttons

* **Status page:** network name, signal strength in dBm with bars and a rating
  (Excellent ≥ −55, Good ≥ −65, Fair ≥ −72, Weak ≥ −80 dBm), IP address and mode,
  RTSP port/path, number of clients and bitrate, CPU and RAM usage, the temperature
  inside the camera (power-management chip, orange from 70 °C, red from 80 °C), the uptime
  in the bottom right corner, and in the header the battery voltage, icon and percentage
  (▲ = charging, yellow **LIM** = long-life charge limit of 4.10 V reached; with long-life
  charging a white tick in the icon marks the limit) or **DC** when there is no battery.
  The percentage comes from the charger chip's own fuel gauge, which calls the 4.10 V
  limit "99–100 %"; the real state of charge there is about 85–90 %.
  About −67 dBm or better is needed for a stable 1080p stream.
* **Any button except power:** WiFi scan page (networks by signal strength, your
  network highlighted, rescans about every 20 s; a network stays listed for 35 s after it was last seen, because the
  WiFi driver sometimes returns only part of the networks; also written to `cam/wifi_scan.txt`).
  Each scan interrupts the stream for about a second (single radio), so the page
  returns to the status page by itself after 5 minutes.
* **Power button short:** screen off/on. **Hold 3 s:** power off.
* The screen turns off (including the backlight) after `SCREEN_TIMEOUT`; any
  button shows it again.

## LEDs (`LED=on`)

| Blue + yellow | Meaning |
|---|---|
| short flash every 4 s | running, nobody is watching |
| soft flash every 2 s | someone is watching the stream |
| fast blinking | no WiFi / no IP address yet |
| double flash every second | error: the stream program is not running |

The red LED is the charger chip's own indicator (blinks while charging) and cannot be switched.

## Files on the SD card

```
cam/autorun.sh      started by the boot hook; starts and supervises everything
cam/config.txt      settings
cam/bin/            camd (stream), camui (screen), camled (LEDs), camnet (network helper),
                    wpa_supplicant + wpa_cli
cam/log.txt         log (LOG=sd; rotated at 512 KB while running, previous part in log.txt.old)
cam/wifi_scan.txt   last WiFi scan
```

## Troubleshooting

* **Logo stays on screen / nothing happens:** the card has no `cam/autorun.sh`, or
  the card is exFAT – the camera only reads **FAT32** (format it in the camera's
  own menu). Look at `cam/log.txt`.
* **No WiFi:** only 2.4 GHz is supported; WPA3-only networks do not work (WPA2/WPA3
  mixed mode does). Set `WIFI_DEBUG_LOG=1` and check `cam/wpa_log.txt`.
* **Back to stock:** set `MODE=stock`, remove the `cam` folder, or restore the boot
  partition with LamaxTool (**Restore original boot**).
* After three starts in a row that did not survive two minutes, the next start stays
  with the stock software once (boot-loop protection).
* The camera is reachable over adb while our software runs (USB is switched on by
  `autorun.sh`).

## Documentation

* [docs/BUILD.md](docs/BUILD.md) – building the camera programs, wpa_supplicant and LamaxTool.exe
* [docs/INSTALL.md](docs/INSTALL.md) – manual installation with adb, recovery
* [docs/HARDWARE.md](docs/HARDWARE.md) – what was found out about the camera (flash layout,
  display, LEDs, encoder, quirks)

## Repository layout

| Path | |
|---|---|
| `src/` | camera programs (C, Android NDK, API 17) |
| `src/debug/` | small probing tools used while reverse engineering |
| `sdcard/cam/` | `autorun.sh`, `config.txt` template (`bin/` is filled by `build.sh`) |
| `boot/` | boot hook (`init.camsd.sh`), image builder (`make_boot.py`), PowerShell flasher |
| `tools/` | LamaxTool (Python/tkinter, Windows + Linux) and its build scripts |
| `third_party/` | wpa_supplicant build script (source is downloaded from w1.fi) |

## License

This project is licensed under the **GNU General Public License v2.0** – see [LICENSE](LICENSE).

## Third-party code and licenses

* `src/vencoder.h` – Allwinner CedarX video encoder API, LGPL-2.1-or-later
  ([allwinner-zh/media-codec](https://github.com/allwinner-zh/media-codec)). The encoder
  library itself is the camera's own `libvencoder.so`, loaded at run time.
* `src/font8x16.h` – VGA 8×16 font from the Linux kernel (`lib/fonts/font_8x16.c`), GPL-2.0.
* wpa_supplicant 2.10 – BSD license, © Jouni Malinen and contributors; built from the
  unmodified release tarball (only a build-time header shim).
* LAMAX is a trademark of its owner. This project is not affiliated with LAMAX or Allwinner.
