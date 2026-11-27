# LAMAX X3.1 Atlas – hardware and firmware notes

Findings from reverse engineering one camera (firmware build `lamaxex_sdv-eng 4.2.2
JDQ39 20211105`, camera firmware version 20210820).

## Hardware

| | |
|---|---|
| SoC | Allwinner V3 (sun8iw8), single Cortex-A7, 128 MB DDR3 (Samsung K4B1G1646) |
| Flash | 8 MB SPI NOR |
| Sensor | Sony IMX175 (`imx175.ko`), via `vfe_v4l2` with in-kernel ISP |
| WiFi | XRadio XR819, 2.4 GHz only, SDIO (`xradio_mac` → `xradio_core` → `xradio_wlan`) |
| PMIC | X-Powers AXP209 on I2C 0x34 |
| Display | 2" 176×220 ILI9225G, MCU (i80) interface, own GRAM; Linux sees 220×176 32 bpp fb0 |
| USB | VID 1F3A / PID 1002, mass storage + adb |

## Software

CamDroid: Android 4.2.2 userspace stripped to the bone, Linux 3.4.39. One process,
`/system/bin/sdv`, is the whole camera UI (MiniGUI). `ro.secure=0`, `ro.debuggable=1`:
adb is a root shell. The stock `sdv` also feeds `/dev/watchdog` (6–8 s timeout).

Flash layout: see [INSTALL.md](INSTALL.md). `system` is squashfs (xz), `cfg` is jffs2
mounted on `/data`, `boot` is an Android boot image (kernel + gzip newc ramdisk,
page 2048, ~28 KB free). `script.bin` (board config) lives in `uboot` at offset 0x32000.

## Video capture and encoding

* `/dev/video0` (driver `sunxi-vfe`) offers NV12 and many other formats at
  3264×2448 … 1280×720. **The frame rate passed to `VIDIOC_S_PARM` selects the sensor
  mode**: 30 fps gives the normal 1080p/720p modes, 25 fps picked a 720p high-speed
  mode. camd always captures at 30 fps and drops frames.
* Sizes the driver lists: 3264×2448, 3264×1836, 3200×1800, 2688×1520, 1920×1088, 1920×1080,
  1632×1224, 1632×918, 1280×720. It picks the largest one that fits the requested size and
  the frame rate's mode, so 2560×1440 gives 1920×1088. Measured with `camtest`:
  2688×1520 at 30 fps (≈27 ms per frame to encode, H.264 level 5.0), 3264×1836 at 15 fps only
  (≈39 ms), 1632×1224 at 15–20 fps, 1280×720 at up to ≈90 fps when 20 fps is requested.
  The full 3264×2448 crashed the camera (not enough memory).
* With 1080 lines the UV plane starts after **1088** luma rows (16-aligned).
* The H.264 encoder is the camera's `libvencoder.so` (CedarX). Its parameter indices
  are 4 higher than in the public `vencoder.h` from 2015: `H264Param = 22`,
  `SPS/PPS = 23` (found by disassembling `H264SetParameter` / `H264GetParameter`).
* `/dev/video0` is held by `mediaserver` while the stock software runs: stop `sdv` and `media`.
* The time stamp is drawn into the encoder's own copy of each frame (luma 235 text,
  16 outline, chroma 128 under it), from a mask rendered once per second; the stream keeps 25 fps.
  The 8×16 font is scaled to about 3.2 % of the frame height in half steps:
  48 px at 1520p (3×), 32 px at 1080p (2×), 24 px at 720p (1.5×).

## Sound

* Microphone: card 0 capture works with plain ALSA ioctls (`src/alsa.c`); the camera's
  `libtinyalsa.so` crashes in `pcm_read` (SIGFPE). Mono 8 kHz is accepted with a period of
  320 frames (40 ms) or more; 160 is rejected. Mixer: `Audio main mic` 1, `Audio capture route
  switch` 1, `Audio record source` mic1, `MIC1 boost AMP gain control` 0–7 (camd: `AUDIO_GAIN`).
  `Audio noise reduced` (hardware noise reduction) stays off. The noise is mostly below 300 Hz.
* The V4L2 frame time stamps are CLOCK_MONOTONIC; camd detects this from the first frame and
  stamps the audio blocks with the same clock, so the RTCP sender reports line both tracks up.
* Speaker on the internal codec (card 0, `audiocodec`); card 1 (`snddaudio`, I2S/TDM) plays
  without errors but is not connected to the speaker.
* init's `startupSound` service runs `/system/bin/startup_music /system/res/others/startup.wav`
  (11025 Hz mono) at about 3 s; it takes any 16-bit PCM WAV path (22050 Hz mono works).
* Playing with plain tinyalsa (`tinyplay`, or `libtinyalsa.so` via dlopen) stalls on card 0:
  the stream is RUNNING but `hw_ptr` stops after 56 frames, writes fail with EIO after
  ~10 s, no DMA interrupts – even with the stock software running, same `pcm_config`
  (1024 × 4, RW_INTERLEAVED). `startup_music` works every time, so `autorun.sh` uses it for
  `cam/startup.wav`. A stalled tinyplay attempt can make the next `startup_music` run fail too.
* The speaker level is `Line Volume` (0–31); `Master Playback Volume` stays 0 and `Audio speaker
  out` reads Off while sound plays. `startup_music` sets Line Volume to 29 when it starts;
  `autorun.sh` sets it back to 31 right after. LamaxTool normalises the WAV and adds a tanh soft
  limiter (≈ +9 dB RMS), which still sounded clean on the camera.

## Clock

* No `date`, `ntpd` or busybox on the camera: `camnet ntp` is a small SNTP client
  (`settimeofday` + `RTC_SET_TIME` on `/dev/rtc0`). The RTC keeps the time over a
  restart, so the time is right before WiFi is up.
* bionic resolves names through `netd` unless `ANDROID_DNS_MODE=local`; in local mode it
  reads `net.dns1`, which dhcpcd does not set (it sets `dhcp.wlan0.dns1`), so `autorun.sh` copies it.
* No time zone database either; bionic does parse POSIX TZ strings (`CET-1CEST,M3.5.0,M10.5.0/3`).

## Display

* fb0 is shown on disp2 layer channel 2 / layer 0 at 0x42600000. Because the panel has
  its own GRAM, a new picture only appears when the layer config is re-applied:
  `DISP_LAYER_SET_CONFIG` (0x47) after each frame (`DISP_LAYER_GET_CONFIG` = 0x48,
  `struct disp_layer_config` 184 bytes, enable at byte 168, channel at 172, layer at 176).
* `FBIOBLANK` and `DISP_LCD_SET_BRIGHTNESS` (0x102) do not switch the backlight off,
  `DISP_LCD_BACKLIGHT_DISABLE` (0x105) returns EPERM. `DISP_DEVICE_SWITCH` (0x0F)
  `{screen 0, type 0}` turns the LCD output and backlight off, `{0, 1}` back on.

## LEDs and buttons

* `/sys/class/leds/led_0` (PE24) is the **blue** LED; `led_1`–`led_4` (PE5, PE6, PC4,
  PC5) have no LED fitted. The blue LED lights when the value is **0**, despite
  `led_0_active_low = 1` in script.bin.
* The **yellow** top LED is on **AXP209 GPIO1**:
  `echo 1 > /sys/devices/platform/axp20-gpio/gpio; echo 1 > …/io; echo 0|1 > …/value`
  (active low; I2C, too slow for PWM).
* The **red** LED is the AXP209 charge LED (hardware).
* Keys: `/dev/input/event0` sunxi-keyboard (codes 139, 114, 115), power key on
  `/dev/input/event2` (axp, code 116).

## Power

`/sys/class/power_supply/battery/{capacity,status,present}`, `usb/online`.
Without a battery `present` is 0 and capacity reads a fake 100 %.
**Charging:** writing `battery/chgen` = 0 clears bit 7 of AXP register 0x33, but the
axp driver sets it again within 1–3 seconds (and `chgen` always reads 1), so charging
cannot be held off. The charge end voltage sticks: `battery/chgmicrovol` = 4100000
gives register 0x33 = 0x90 (4.10 V, ≈ 85–90 %); the stock value is 4200000 (0xD0).
The AXP209 offers 4.10 / 4.15 / 4.20 / 4.36 V only – there is no percentage limit.

**Power-off voltage:** the AXP209 cuts the power at VOFF, register 0x31 bits 2–0
(2.6 V + n × 0.1 V); stock 0x03 = 2.9 V. With `BATTERY_LONG_LIFE=1` autorun.sh sets 3.1 V
as a backstop, and camui powers off cleanly (`reboot -p`) at 10 % or below 3.45 V for 30 s.
On external power with long-life charging the cell idles at ~4.05 V: charging stops at
4.10 V and only restarts after the voltage has dropped (status *Not charging*, 0 mA).

**Power-on:** the camera switches itself on when external (USB) power is connected
(tested), so it restarts by itself after a power cut. There is no wake-on-WLAN: when it
is off, the WiFi chip has no power.

**AXP209 registers** can be read through the driver's own sysfs files in
`/sys/bus/i2c/devices/0-0034/`: write a register number below 0x100 (hex) to
`axp20_reg` or `axp20_regs`, then read it back (`REG[33]=d0` /
`REG[0x5e]=0x7e,REG[0x5f]=0x4`). Writing a 16-bit value `RRVV` to **`axp20_reg`** writes
VV to register RR – be careful (the same write to `axp20_regs` does nothing). (`/sys/class/axppower/regdebug` only stores a number, it reads nothing.)

## Temperature

* There is no `thermal_zone`/`hwmon`, and `battery/temp` is a constant 30.0 °C
  (`pmu_temp_enable = 0` in script.bin).
* **AXP209 internal temperature** works: registers 0x5E/0x5F, 12 bit,
  T = raw × 0.1 − 144.7 °C (≈ 57 °C inside a running camera at room temperature).
  This is what `camui` shows; it tracks the heat inside the housing.
* The V3 SoC thermal sensor is not usable: `/dev/mem` is not in the kernel; the BSP's
  `/sys/class/sunxi_dump` (`dump` = `echo 0xADDR > dump; cat dump`, `write` =
  `echo "0xADDR 0xVAL" > write`) can reach registers, but after enabling the THS bus
  clock (CCU 0x01C20068 bit 8) and reset (0x01C202D0 bit 8) the H3-style THS block at
  0x01C25000 still reads all zeros and ignores writes – the V3 appears not to have it
  there (mainline Linux has no V3/V3s thermal driver either).

## USB and SD card

* The stock app switches the USB gadget on; without it the port stays dead. Writing
  the `android_usb` sysfs attributes (as in `init.sun8i.usb.rc`) brings adb back.
* `vold` mounts the card at `/mnt/extsd` about 3 s after boot. Only **vfat** is in the
  kernel – exFAT cards are not mounted.
* `LD_LIBRARY_PATH` contains `/mnt/extsd/debug_lib`, but all libraries are loaded
  before the card is mounted, so it cannot be used as a boot hook.
* WiFi scanning while streaming costs about one second of stream per scan (single radio).
