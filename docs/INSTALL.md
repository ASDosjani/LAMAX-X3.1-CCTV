# Manual installation (without LamaxTool)

Requirements: Android platform-tools (`adb`), Python 3, a USB data cable. The examples
use PowerShell; on Linux the same `adb` and `python3 boot/make_boot.py` commands work
(the flashing steps are what `boot/flash_boot.ps1` and LamaxTool do: push the image to
`/dev`, `cat` it to `/dev/block/mtdblock1`, `sync`, drop caches, `cmp`).
Connect the camera and select **mass storage** on its screen; `adb devices` must list it.
The stock firmware gives a root shell over adb.

## 1. Back up the whole flash

```powershell
$out = "$env:USERPROFILE\Documents\lamax_backup_$(adb get-serialno)"
mkdir $out
foreach ($i in 0..7) { adb pull /dev/block/mtdblock$i "$out\mtdblock$i.bin" }
adb shell cat /proc/mtd > "$out\mtd.txt"
```

Expected partition table (8 MB SPI NOR):

```
mtd0 uboot 256K | mtd1 boot 2.5M | mtd2 system 4.5M | mtd3 cfg 320K
mtd4 boot_logo 64K | mtd5 shutdown_logo 64K | mtd6 env 64K | mtd7 private 64K
```

Pull each partition twice and compare the files – they must be identical.

## 2. Build and write the boot hook

```powershell
python boot\make_boot.py "$out\mtdblock1.bin" boot\boot_camsd.img
.\boot\flash_boot.ps1 -Image boot\boot_camsd.img -Backup "$out\mtdblock1.bin"
```

`make_boot.py` adds `/init.camsd.sh` and a `camsd` service to the ramdisk's `init.rc`;
the kernel is copied unchanged and the result is checked to fit the partition.
`flash_boot.ps1` writes `/dev/block/mtdblock1`, drops the page cache, compares the
flash with the image and writes the backup back if they differ. It does not reboot.

## 3. Prepare the SD card

The card must be **FAT32** (64 GB cards come as exFAT: format the card in the camera's
own menu). Copy `sdcard/cam` (with the built `bin/`) to the card root and edit
`cam/config.txt`. Reboot the camera.

## Recovery

* **Stock behaviour without touching the flash:** `MODE=stock` in `config.txt` or remove `cam/`.
* **Original boot partition:** `.\boot\flash_boot.ps1 -Image "$out\mtdblock1.bin" -Backup "$out\mtdblock1.bin"`
  (or LamaxTool → Restore original boot).
* **Camera does not boot at all:** the bootloader (mtd0) is never written, so the
  Allwinner FEL USB mode is still available; with `sunxi-fel` the SPI flash can be
  rewritten from the backup. See the [linux-sunxi FEL documentation](https://linux-sunxi.org/FEL).
