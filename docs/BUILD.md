# Building

This page walks through a complete build from a fresh Windows machine: the camera
programs, wpa_supplicant, LamaxTool for Windows and Linux, and the release package.
Building on a plain Linux machine works the same way – skip the WSL part and run the
`bash` commands directly.

## What gets built

| Output | Built by | Runs on | Purpose |
|---|---|---|---|
| `camd` | `build.sh` | camera | camera → H.264 → RTSP server, date/time in the picture (`osd.c`), watchdog |
| `camui` | `build.sh` | camera | screen, buttons, WiFi scan page, battery/CPU/temperature |
| `camled` | `build.sh` | camera | status LEDs |
| `camd` audio | `src/audio.c`, `src/alsa.c` | camera | microphone → G.711 → RTSP track 2 (`AUDIO=1`) |
| `camcfg` | `build.sh` | camera | configuration over WiFi for LamaxTool (HTTP + Digest, only with `ADMIN_PASSWORD`); `config.txt` encrypted with `src/seal.c` (SHA-256, HMAC, PBKDF2, ChaCha20 – checked against the FIPS 180 / RFC 4231 / RFC 8439 test vectors; LamaxTool has the same in Python) |
| `camnet` | `build.sh` | camera | network helper for `autorun.sh` (static IP, config values, SNTP clock) |
| `camtest` | `build.sh` | camera | capture + encode test (development only) |
| `wpa_supplicant`, `wpa_cli` | `third_party/build_wpa.sh` | camera | WiFi client |
| `LamaxTool.exe` | `tools/build_exe.ps1` | Windows | backup / flash / SD card tool |
| `LamaxTool` | `tools/build_linux.sh` | Linux x86-64 | same tool for Linux |
| `LamaxTool.app` | `tools/build_macos.sh` / GitHub Actions | macOS (Apple Silicon) | same tool for macOS |
| `release-<ver>/` | `tools/make_release.ps1` | – | files for a GitHub release |

`build.sh` copies the camera programs into `sdcard/cam/bin/`; LamaxTool embeds the
whole `sdcard/cam` folder and the boot hook, so build the camera programs **first**.

## 1. Prerequisites

### Windows

* Windows 10 2004+ or Windows 11
* **WSL with Ubuntu** (22.04 was used). In an administrator PowerShell:
  ```powershell
  wsl --install -d Ubuntu-22.04
  ```
  Reboot when asked, start "Ubuntu 22.04" once and create the Linux user.
* **Python 3.9+** from python.org (with the default *tcl/tk* option) – for LamaxTool.
* **Git** (optional, to clone the repository).
* Android **platform-tools** (adb) only for testing on a camera; LamaxTool downloads
  it by itself when needed.

### Inside WSL / Linux

```bash
sudo apt update
sudo apt install -y curl unzip make file python3 python3-pip python3-tk python3-venv
```

`python3-tk` and `python3-venv` are only needed for the Linux LamaxTool build.

## 2. Get the source

From WSL the Windows drives are under `/mnt/c`. Working directly in the Windows folder
is fine (that is how this project was developed):

```bash
cd /mnt/c/Users/<you>/Documents
git clone https://github.com/<owner>/LAMAX-X3.1-CCTV.git
cd LAMAX-X3.1-CCTV
```

The shell scripts must keep LF line endings – `.gitattributes` takes care of that even
with Git for Windows' default `core.autocrlf=true`. If you copy files around by other
means and get `/bin/bash^M: bad interpreter`, convert them with `sed -i 's/\r$//' file`.

## 3. Android NDK r21e (once)

The camera runs Android 4.2.2 (API 17) on an armv7 Cortex-A7. NDK r21e is the last
NDK that targets API 16/17 with clang.

```bash
mkdir -p ~/lamax/toolchain && cd ~/lamax/toolchain
curl -LO https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip
sha1sum android-ndk-r21e-linux-x86_64.zip   # c3ebc83c96a4d7f539bd72c241b2be9dcd29bda9
unzip -q android-ndk-r21e-linux-x86_64.zip && rm android-ndk-r21e-linux-x86_64.zip
```

Elsewhere? Set `NDK=/path/to/android-ndk-r21e` for every build script.

## 4. Camera programs

```bash
cd /mnt/c/Users/<you>/Documents/LAMAX-X3.1-CCTV
bash build.sh
```

* Compiler: `armv7a-linux-androideabi17-clang`, `-O2 -march=armv7-a -mfpu=neon
  -mfloat-abi=softfp -fPIE`, linked `-pie -ldl`, stripped.
* Results in `out/`, the camera's set is copied to `sdcard/cam/bin/`.
* No link-time dependency on the camera's private libraries: the hardware H.264
  encoder (`/system/lib/libvencoder.so`) is opened with `dlopen` at run time, with the
  parameter indices used by the camera's library version (see HARDWARE.md).
* Takes a few seconds; rerun after every source change.

## 5. wpa_supplicant (once, or after changing its configuration)

```bash
bash third_party/build_wpa.sh
bash build.sh            # copies the new binaries to sdcard/cam/bin/
```

The script downloads `wpa_supplicant-2.10.tar.gz` from w1.fi (SHA-256 checked) into
`~/lamax/build/wpa`, replaces the private netlink header with the NDK's real ones,
and builds with:

* WEXT driver only (the XR819 driver supports it, so no libnl is needed),
* internal crypto (WPA/WPA2-PSK and open networks; no EAP, no WPA3/SAE),
* control interface on, client sockets in `/dev/cam` (Android has no `/tmp`).

The stock firmware still references `/system/bin/wpa_supplicant` in `init.rc`, but the
binary was removed from the camera, which is why it is built here.

## 6. LamaxTool

### Windows (PowerShell, in the repository folder)

```powershell
.\tools\build_exe.ps1
```

Creates a virtual environment in `%TEMP%\lamaxtool-venv` (short path – deep paths can
break pip), installs PyInstaller and builds `dist\LamaxTool.exe` (single file,
about 11 MB, no console window).

### Linux (x86-64)

```bash
bash tools/build_linux.sh
```

Builds `dist/LamaxTool` (about 12 MB). Uses a venv in `~/.cache/lamaxtool-venv`, or a
per-user pip install if `python3-venv` is missing. PyInstaller binaries run on systems
with the same or a newer glibc than the build machine (Ubuntu 22.04 → glibc 2.35), so
build on the oldest distribution you want to support.

### macOS (Apple Silicon)

PyInstaller cannot cross-compile, so the Mac app is built on a Mac – or by GitHub:
the workflow `.github/workflows/build-macos.yml` builds the camera programs on Linux
(NDK r21e + wpa_supplicant, exactly as above) and then `LamaxTool.app` on an arm64
macOS runner. Start it from the Actions tab (**Run workflow**); it also runs on every push
to `main` that touches the tool or the camera programs. The result is the artifact
`LamaxTool-macOS-arm64` (a zip of `LamaxTool.app`).

On a Mac with the camera programs already in `sdcard/cam/bin` (copied from a Linux/WSL build):

```bash
bash tools/build_macos.sh     # -> dist/LamaxTool.app, dist/LamaxTool-macOS-arm64.zip
```

Needs a python3 with tkinter (python.org installer, or `brew install python-tk`). The app is
only ad-hoc signed (PyInstaller does that), so Gatekeeper asks once: right-click → Open.
Its data folder is `~/LamaxTool` (backups, platform-tools, `LamaxTool_settings.txt`),
because the app bundle itself is read-only.

### Committed builds

`dist/LamaxTool.exe` and `dist/LamaxTool` are committed (everything else in `dist/` is
ignored), so a fresh clone works without building. After changing `tools/`, `boot/`,
`sdcard/` or the camera programs, rebuild **both** and commit them with the change –
they contain the camera programs and `autorun.sh` from the time they were built.
`git add` keeps the Linux file's executable bit only if it was already set; on Windows:
`git update-index --chmod=+x dist/LamaxTool`.

### From source (any OS with Python 3.9+ and tkinter)

```bash
python3 tools/lamax_tool.py
```

Runtime requirements of the tool: `adb` (found on PATH, in `platform-tools/` next to
the program, in `~/Downloads/platform-tools`, or downloaded on first use). The WiFi
scan runs on the camera, so nothing else is needed on the computer.
On Linux adb needs a udev rule for USB vendor `1f3a` (see README).

## 7. Release package

```powershell
.\tools\make_release.ps1 -Version 1.0.0
```

Collects into `dist\release-1.0.0\`:

* `LamaxTool.exe` and `LamaxTool` (whichever has been built),
* `sdcard.zip` – the `cam` folder (`autorun.sh`, the `config.txt` template, `bin/`) for
  preparing cards by hand,
* `SHA256SUMS.txt`.

Upload these to a GitHub release. **Never publish** flash backups (`backups/`), boot
images (`*.img`) or a filled-in `config.txt` – they contain the manufacturer's
firmware, device identifiers or WiFi passwords (`.gitignore` excludes them).

## 8. Testing on a camera

The camera must be connected over USB with adb working (stock firmware: choose *mass
storage*; with this software installed adb is always on).

**Quick capture/encoder test without the SD card** (stock firmware):

```bash
adb shell mkdir -p /dev/cam
adb push out/camtest /dev/cam/
adb shell 'stop sdv; stop media; /dev/cam/camtest -W 1920 -H 1080 -n 90 -o /dev/cam/t.h264'
adb pull /dev/cam/t.h264
```

The stock app feeds a 6–8 s hardware watchdog: after `stop sdv` the camera reboots
unless something writes `/dev/watchdog` (camd does).

**Replace a program on a running camera** (our software installed, SD mounted):

```bash
adb push sdcard/cam/bin/camui /mnt/extsd/cam/bin/camui      # for the next boot
adb push sdcard/cam/bin/camui /dev/cam/camui.new
adb shell 'for p in /proc/[0-9]*; do case "$(cat $p/cmdline)" in /dev/cam/camui*) kill ${p#/proc/};; esac; done;
           rm /dev/cam/camui; cp /dev/cam/camui.new /dev/cam/camui; chmod 755 /dev/cam/camui'
```

`autorun.sh` restarts a missing program within 3 s. For `autorun.sh` itself, push it
to the card, check it with `adb shell sh -n /mnt/extsd/cam/autorun.sh` and reboot.

**Screenshot of the camera screen:** `adb shell cat /dev/graphics/fb0 > fb.raw` gives a
220×176 BGRA dump (stride 880 bytes; page 0 is shown).

**Logs:** `cam/log.txt` on the card (`adb shell cat /mnt/extsd/cam/log.txt`).

**Debug tools** in `src/debug` (`disptest` – raw `/dev/disp` ioctls, `fsprobe` – SD card
filesystem type): `bash tools/build_debug.sh disptest`.

## 9. Clean

```bash
rm -rf out build sdcard/cam/bin
```

`dist/` is left alone: besides the committed builds it may hold `backups/` (your flash
backups, if LamaxTool was run from there) and `LamaxTool_settings.txt`.
