#!/system/bin/sh
# LAMAX boot hook (lives in the boot ramdisk, started by init.rc).
# If the SD card holds cam/autorun.sh, hand control to it; otherwise do
# nothing and the stock camera software keeps running unchanged.
# The stock app (sdv) mounts the SD card, so wait for it for up to 40 s.
i=0
while [ $i -lt 40 ]; do
    if [ -f /mnt/extsd/cam/autorun.sh ]; then
        exec /system/bin/sh /mnt/extsd/cam/autorun.sh
    fi
    sleep 1
    i=$((i + 1))
done
