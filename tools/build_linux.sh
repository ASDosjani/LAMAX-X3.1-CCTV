#!/bin/bash
# Build dist/LamaxTool (single-file Linux binary) with PyInstaller.
# Needs python3 with venv and tkinter (Debian/Ubuntu: sudo apt install python3-venv python3-tk).
# The camera programs must be built first (bash build.sh) so that sdcard/cam/bin is filled.
# Running from source works too:  python3 tools/lamax_tool.py
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
[ -f sdcard/cam/bin/camd ] || { echo "sdcard/cam/bin is empty - run build.sh first (see docs/BUILD.md)"; exit 1; }
python3 -c 'import tkinter' 2>/dev/null || { echo "tkinter missing: sudo apt install python3-tk"; exit 1; }
VENV=${VENV:-$HOME/.cache/lamaxtool-venv}
if [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV" >/dev/null 2>&1; then
    PY="$VENV/bin/python"
    "$PY" -m pip install --quiet --upgrade pip pyinstaller sounddevice paramiko
else
    # no python3-venv package: fall back to a per-user install
    rm -rf "$VENV"
    PY=python3
    "$PY" -m pip install --quiet --user pyinstaller sounddevice paramiko
fi
"$PY" -m PyInstaller --noconfirm --clean --onefile --windowed --name LamaxTool --exclude-module numpy \
    --distpath dist --workpath build/pyinstaller-linux --specpath build \
    --add-data "$ROOT/boot/make_boot.py:boot" \
    --add-data "$ROOT/boot/init.camsd.sh:boot" \
    --add-data "$ROOT/sdcard/cam:sdcard/cam" \
    --add-data "$ROOT/ORIGINAL_FW/mtdblock1_boot.bin:ORIGINAL_FW" \
    --add-data "$ROOT/ORIGINAL_FW/SHA256SUMS:ORIGINAL_FW" \
    tools/lamax_tool.py
echo "Built: $ROOT/dist/LamaxTool"
