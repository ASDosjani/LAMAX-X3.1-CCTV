#!/bin/bash
# Build the camera programs with Android NDK r21e (API 17 = Android 4.2, armeabi-v7a).
# Run inside WSL:  bash build.sh   ->  output in ./out/
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$HOME/lamax/toolchain/android-ndk-r21e}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC="$TC/armv7a-linux-androideabi17-clang"
CFLAGS="-O2 -Wall -Wno-unused-parameter -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=softfp -fPIE"
LDFLAGS="-pie -ldl -Wl,--hash-style=sysv"
mkdir -p out

build() { # name sources...
    local name=$1; shift
    echo "  CC $name"
    $CC $CFLAGS -o out/$name "$@" $LDFLAGS
    $TC/llvm-strip out/$name
}

build camtest src/camtest.c src/v4l2cap.c src/h264enc.c
[ -f src/camd.c ] && build camd src/camd.c src/v4l2cap.c src/h264enc.c src/rtsp.c src/md5.c src/config.c src/osd.c src/audio.c src/alsa.c
[ -f src/camui.c ] && build camui src/camui.c src/fb.c src/wpactrl.c src/config.c
[ -f src/camnet.c ] && build camnet src/camnet.c src/config.c src/wpactrl.c
[ -f src/camled.c ] && build camled src/camled.c src/config.c src/wpactrl.c -lm
[ -f src/camcfg.c ] && build camcfg src/camcfg.c src/config.c src/wpactrl.c src/md5.c src/seal.c src/alsa.c

# assemble the SD card folder: sdcard/cam/bin
mkdir -p sdcard/cam/bin
for b in camd camui camnet camled camcfg wpa_supplicant wpa_cli; do [ -f out/$b ] && cp out/$b sdcard/cam/bin/; done
ls -la out sdcard/cam/bin
