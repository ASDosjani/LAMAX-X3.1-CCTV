#!/bin/bash
# Build wpa_supplicant + wpa_cli for the camera (Android 4.2 bionic, armv7).
# WEXT driver only (no libnl needed); WPA/WPA2-PSK with the internal crypto.
set -e
cd "$(dirname "$0")"
HERE=$PWD
NDK=${NDK:-$HOME/lamax/toolchain/android-ndk-r21e}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
VER=2.10
WORK=$HOME/lamax/build/wpa
mkdir -p "$WORK" && cd "$WORK"
if [ ! -d wpa_supplicant-$VER ]; then
    curl -sSfLO https://w1.fi/releases/wpa_supplicant-$VER.tar.gz
    echo "20df7ae5154b3830355f8ab4269123a87affdea59fe74fe9292a91d0d7e17b2f  wpa_supplicant-$VER.tar.gz" | sha256sum -c -
    tar xzf wpa_supplicant-$VER.tar.gz
fi
# The NDK ships real netlink headers; use them instead of wpa_supplicant's private copy.
cat > wpa_supplicant-$VER/src/drivers/priv_netlink.h <<'EOF'
#ifndef PRIV_NETLINK_H
#define PRIV_NETLINK_H
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif
#ifndef IFF_DORMANT
#define IFF_DORMANT 0x20000
#endif
#endif
EOF
cd wpa_supplicant-$VER/wpa_supplicant
cat > .config <<'EOF'
CONFIG_DRIVER_WEXT=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_CRYPTO=internal
CONFIG_NO_RANDOM_POOL=y
CONFIG_ELOOP_POLL=y
CONFIG_OS=unix
CONFIG_NO_STDOUT_DEBUG=
CONFIG_DEBUG_FILE=y
EOF
make clean >/dev/null 2>&1 || true
make -j8 CC="$TC/armv7a-linux-androideabi17-clang" \
     EXTRA_CFLAGS="-Os -fPIE -Wno-unused-parameter -DCONFIG_CTRL_IFACE_CLIENT_DIR=\\\"/dev/cam\\\"" \
     LDFLAGS="-pie" LIBS="" LIBS_c="" wpa_supplicant wpa_cli 2>&1 | tail -n 15
$TC/llvm-strip wpa_supplicant wpa_cli
mkdir -p "$HERE/../out"
cp wpa_supplicant wpa_cli "$HERE/../out/"
ls -la "$HERE/../out/wpa_supplicant" "$HERE/../out/wpa_cli"
