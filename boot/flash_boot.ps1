# Write a boot image to the camera's boot partition (mtd1) over adb, verify it
# by reading the flash back, and restore the original automatically if the
# verification fails. Does NOT reboot the camera.
#
#   .\flash_boot.ps1 -Image boot_camsd.img -Backup <backup>\mtdblock1.bin   install the SD-card hook
#   .\flash_boot.ps1 -Image <backup>\mtdblock1.bin -Backup <same file>      restore the original
# -Backup is this camera's original boot partition; it is written back if verification fails.
param(
    [Parameter(Mandatory)] [string]$Image,
    [Parameter(Mandatory)] [string]$Backup
)
$ErrorActionPreference = 'Stop'
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) { $adb = Join-Path $env:USERPROFILE 'Downloads\platform-tools\adb.exe' }
if (-not (Test-Path $adb)) { throw 'adb not found: install Android platform-tools and put adb on PATH' }
$PART = 2621440
$Image = (Resolve-Path $Image).Path
$Backup = (Resolve-Path $Backup).Path
foreach ($f in $Image, $Backup) {
    if (-not (Test-Path $f)) { throw "missing $f" }
    if ((Get-Item $f).Length -ne $PART) { throw "$f must be exactly $PART bytes" }
}
$img = [IO.File]::ReadAllBytes($Image)
if ([Text.Encoding]::ASCII.GetString($img, 0, 8) -ne 'ANDROID!') { throw "$Image is not an Android boot image" }

& $adb wait-for-device
$mtd = (& $adb shell 'cat /proc/mtd') -join "`n"
if ($mtd -notmatch 'mtd1: 00280000 00001000 "boot"') { throw "unexpected partition table:`n$mtd" }

& $adb push $Image /dev/boot_new.img | Out-Null
& $adb push $Backup /dev/boot_orig.img | Out-Null
$script = @'
cmp /dev/boot_new.img /dev/block/mtdblock1 >/dev/null 2>&1 && { echo ALREADY; exit 0; }
cat /dev/boot_new.img > /dev/block/mtdblock1
sync
echo 3 > /proc/sys/vm/drop_caches
if cmp /dev/boot_new.img /dev/block/mtdblock1; then
    echo VERIFIED
else
    echo VERIFY_FAILED_RESTORING
    cat /dev/boot_orig.img > /dev/block/mtdblock1
    sync
    echo 3 > /proc/sys/vm/drop_caches
    cmp /dev/boot_orig.img /dev/block/mtdblock1 && echo RESTORED || echo RESTORE_FAILED
fi
rm -f /dev/boot_new.img /dev/boot_orig.img
'@
$tmp = Join-Path $env:TEMP 'flash_boot.sh'
[IO.File]::WriteAllText($tmp, $script.Replace("`r`n", "`n"))
& $adb push $tmp /dev/flash_boot.sh | Out-Null
Remove-Item $tmp
$r = (& $adb shell 'sh /dev/flash_boot.sh 2>&1; rm -f /dev/flash_boot.sh') -join "`n"
Write-Output $r
if ($r -match 'VERIFIED|ALREADY') { Write-Output 'OK: the boot partition now matches the image. Reboot the camera to use it.' }
else { Write-Output 'NOT OK: see the messages above. Do not reboot until this is sorted out.'; exit 1 }
