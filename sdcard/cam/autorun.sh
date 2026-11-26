#!/system/bin/sh
# LAMAX X3.1 security camera – started from the SD card by the boot hook
# (/init.camsd.sh in the modified boot partition). Can also be run by hand:
#   sh /mnt/extsd/cam/autorun.sh [sd-dir]
# Only toolbox commands exist on the camera (no grep/sed/awk/killall/printf).

SD=${1:-/mnt/extsd/cam}
RUN=/dev/cam
CFG=$SD/config.txt

mkdir -p $RUN
# stop leftovers of a previous run (when started by hand)
for p in /proc/[0-9]*; do
    case "$(cat $p/cmdline 2>/dev/null)" in $RUN/*) kill ${p#/proc/} 2>/dev/null;; esac
done
# binaries run from RAM (the SD card may be mounted noexec)
for f in $SD/bin/*; do
    n=$RUN/${f##*/}
    rm -f $n
    cp $f $n && chmod 755 $n
done
get() { $RUN/camnet get $CFG "$1" "$2"; }

# --- log --------------------------------------------------------------------
# LOG=sd  : cam/log.txt on the SD card, at most 2 x 512 KB (log.txt + log.txt.old)
# LOG=ram : /dev/cam/log.txt in RAM only (lost at power-off), at most 2 x 128 KB
# LOG=off : no log (default)
case "$(get LOG off)" in
    sd)  LOG=$SD/log.txt; LOG_MAX=524288 ;;
    ram) LOG=$RUN/log.txt; LOG_MAX=131072 ;;
    *)   LOG=/dev/null; LOG_MAX=0 ;;
esac
# Every program started from here appends to the same file (O_APPEND), so it can
# be rotated at run time by copying it to .old and truncating it in place.
rotate_file() {   # file max_bytes
    [ "$2" -gt 0 ] && [ -f "$1" ] || return
    set -- "$1" "$2" $(ls -l "$1")
    if [ "${6:-0}" -gt "$2" ]; then
        cp "$1" "$1.old"
        : > "$1"
    fi
}
log_rotate() {
    rotate_file $LOG $LOG_MAX
    rotate_file $SD/wpa_log.txt 1048576       # WIFI_DEBUG_LOG=1 (appends, so it can be truncated)
}
log_rotate
exec >>$LOG 2>&1
ts() { set -- $(cat /proc/uptime); echo "[${1}s]"; }   # no `date` on the camera
echo "===== start $(ts) ====="

MODE=$(get MODE camera)
if [ "$MODE" != "camera" ]; then
    echo "MODE=$MODE -> stock firmware stays in charge"
    exit 0
fi

# boot-loop protection: if 3 starts in a row did not survive 2 minutes,
# leave the stock software running once (the counter lives in /data).
BOOTS=$(cat /data/camsd_boots 2>/dev/null)
BOOTS=${BOOTS:-0}
if [ "$BOOTS" -ge 3 ]; then
    echo "$(ts) $BOOTS failed starts in a row -> staying with stock software this time"
    echo 0 > /data/camsd_boots
    exit 0
fi
echo $((BOOTS + 1)) > /data/camsd_boots
sync

# --- take over from the stock software -----------------------------------
# sdv feeds the 6 s hardware watchdog; camd takes over within ~1 s.
stop sdv
stop media
# sdv normally switches the USB port on when a PC is plugged in; without it the
# port stays off. Program the gadget directly (same steps as init.sun8i.usb.rc)
# so adb is always reachable; setprop alone does nothing if the value is unchanged.
usb_on() {
    U=/sys/class/android_usb/android0
    echo 0 > $U/enable
    echo 1f3a > $U/idVendor
    echo 1002 > $U/idProduct
    echo ef > $U/bDeviceClass
    echo 02 > $U/bDeviceSubClass
    echo 01 > $U/bDeviceProtocol
    echo mass_storage,adb > $U/functions
    echo 1 > $U/enable
    setprop sys.usb.state mass_storage,adb
    start adbd
    echo "$(ts) usb: enable=$(cat $U/enable) functions=$(cat $U/functions) state=$(cat $U/state 2>/dev/null)"
}
usb_on
$RUN/camd $CFG &
CAMD=$!
echo "camd pid $CAMD"

# --- WiFi -----------------------------------------------------------------
# The stock app normally loads the XR819 driver (mac -> core -> wlan); after
# an early takeover nobody has done it yet. insmod of an already loaded
# module just fails harmlessly.
M=/system/vendor/modules
if [ ! -d /sys/class/net/wlan0 ]; then
    insmod $M/xradio_mac.ko
    insmod $M/xradio_core.ko
    insmod $M/xradio_wlan.ko
fi
i=0
while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done
[ -d /sys/class/net/wlan0 ] && echo "$(ts) wlan0 ready" || echo "$(ts) ERROR: wlan0 did not appear"
ifconfig wlan0 up
$RUN/camnet wpaconf $CFG $RUN/wpa.conf
start_wpa() {
    rm -rf $RUN/wpa
    if [ "$(get WIFI_DEBUG_LOG 0)" = "1" ]; then
        $RUN/wpa_supplicant -B -i wlan0 -D wext -c $RUN/wpa.conf -d -f $SD/wpa_log.txt
    else
        $RUN/wpa_supplicant -B -i wlan0 -D wext -c $RUN/wpa.conf -P $RUN/wpa.pid
    fi
}
start_wpa

IP_MODE=$(get IP_MODE dhcp)
apply_ip() {
    if [ "$IP_MODE" = "static" ]; then
        $RUN/camnet static wlan0 $(get IP_ADDRESS) $(get IP_NETMASK 255.255.255.0) $(get IP_GATEWAY)
        setprop net.dns1 $(get IP_DNS 8.8.8.8)
    else
        mkdir -p /data/misc/dhcp
        /system/bin/dhcpcd -ABKL wlan0 &
        DHCP=$!
    fi
}

# --- screen + buttons -----------------------------------------------------
$RUN/camui $CFG &
CAMUI=$!

# --- LEDs (LED=on/off) ------------------------------------------------------
$RUN/camled $CFG &
CAMLED=$!

# --- battery charge limit -------------------------------------------------
# The AXP209 charger has no percentage limit of its own, so switch charging
# The AXP209 charger has no percentage limit, and the axp driver switches
# charging back on within seconds if it is turned off. What sticks is the
# charge end voltage: 4.10 V (about 85-90 %) instead of 4.20 V spares the
# battery when the camera is always on external power.
BAT=/sys/class/power_supply/battery
if [ "$(get BATTERY_LONG_LIFE 1)" = "1" ]; then CHG_UV=4100000; else CHG_UV=4200000; fi
[ -w $BAT/chgen ] && echo 1 > $BAT/chgen        # undo anything older versions did
# Hardware cut-off (AXP209 VOFF, reg 0x31 bits 2-0): 2.9 V stock, 3.1 V in long-life mode
# as a backstop. The normal early shutdown (10 % / 3.45 V) is done by camui.
AXP=/sys/bus/i2c/devices/0-0034/axp20_reg      # writes need axp20_reg (RRVV)
if [ -w $AXP ]; then
    if [ "$(get BATTERY_LONG_LIFE 1)" = "1" ]; then echo 3105 > $AXP; else echo 3103 > $AXP; fi
fi
charge_limit() {
    [ -w $BAT/chgmicrovol ] || return
    if [ "$(cat $BAT/chgmicrovol)" != "$CHG_UV" ]; then
        echo $CHG_UV > $BAT/chgmicrovol
        echo "$(ts) battery $(cat $BAT/capacity)%: charge end voltage set to ${CHG_UV} uV"
    fi
}

# --- network time (for the time stamp in the picture) ----------------------
# SNTP from NTP_SERVER, then the router (many routers answer NTP). Every 6 hours
# when it works, every minute until it does. The RTC keeps the time over a restart.
NTP_NEXT=0
ntp_sync() {
    if [ "$IP_MODE" = "static" ]; then GW=$(get IP_GATEWAY); else
        GW=$(getprop dhcp.wlan0.gateway)
        D=$(getprop dhcp.wlan0.dns1); [ -n "$D" ] && setprop net.dns1 "$D"
    fi
    if OUT=$($RUN/camnet ntp $(get NTP_SERVER pool.ntp.org) $GW 2>&1); then
        NTP_NEXT=$((LOOPS + 7200))
    else
        NTP_NEXT=$((LOOPS + 20))
    fi
    echo "$(ts) $OUT"
}

# --- supervisor -----------------------------------------------------------
alive() { [ -n "$1" ] && [ -d /proc/$1 ]; }
LINKED=0
LOOPS=0
while true; do
    LOOPS=$((LOOPS + 1))
    [ $((LOOPS % 100)) = 0 ] && log_rotate        # every ~5 minutes
    if [ $LOOPS = 5 ] || [ $LOOPS = 20 ]; then
        U=/sys/class/android_usb/android0
        echo "$(ts) usb: enable=$(cat $U/enable) state=$(cat $U/state 2>/dev/null) adbd=$(getprop init.svc.adbd)"
    fi
    if [ $LOOPS = 40 ]; then          # ~2 minutes up: this start counts as good
        echo 0 > /data/camsd_boots
        sync
        echo "$(ts) running stable"
    fi
    if ! alive $CAMD; then
        echo "$(ts) camd exited, restarting"
        $RUN/camd $CFG &
        CAMD=$!
    fi
    if ! alive $CAMUI; then
        $RUN/camui $CFG &
        CAMUI=$!
    fi
    # camled exits by itself when LED=off; only restart it when LEDs are on
    if [ "$(get LED on)" != "off" ] && [ "$(get LED_MODE status)" = "status" ] && ! alive $CAMLED; then
        $RUN/camled $CFG &
        CAMLED=$!
    fi
    if [ ! -S $RUN/wpa/wlan0 ]; then
        echo "$(ts) wpa_supplicant not running, restarting"
        start_wpa
    fi
    STATE=$($RUN/camnet state $RUN/wpa/wlan0)
    if [ "$STATE" = "COMPLETED" ]; then
        if [ $LINKED = 0 ]; then
            echo "$(ts) WiFi connected"
            apply_ip
            LINKED=1
            IPLOGGED=0
            echo 0 > /data/camsd_boots     # WiFi works: this start counts as good
        fi
        if [ "$IPLOGGED" = 0 ] && $RUN/camnet show wlan0 > $RUN/ip.txt 2>&1; then
            echo "$(ts) $(cat $RUN/ip.txt)"
            IPLOGGED=1
        fi
        [ "$IPLOGGED" = 1 ] && [ $LOOPS -ge $NTP_NEXT ] && ntp_sync
    elif [ $LINKED = 1 ]; then
        echo "$(ts) WiFi lost ($STATE)"
        LINKED=0
        [ -n "$DHCP" ] && kill $DHCP 2>/dev/null
        DHCP=
    fi
    charge_limit
    sleep 3
done
