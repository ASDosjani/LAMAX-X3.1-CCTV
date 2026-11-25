#!/bin/bash
# Build one of the small probing tools in src/debug (not needed for normal use):
#   bash tools/build_debug.sh disptest      -> out/disptest
cd "$(dirname "$0")/.."
NDK=${NDK:-$HOME/lamax/toolchain/android-ndk-r21e}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -n "$1" ] || { echo "usage: $0 <name of src/debug/*.c>"; exit 1; }
mkdir -p out
$TC/armv7a-linux-androideabi17-clang -O2 -Wall -fPIE -pie -o "out/$1" "src/debug/$1.c" \
    && $TC/llvm-strip "out/$1" && echo "built out/$1"
