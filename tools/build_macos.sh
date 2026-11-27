#!/bin/bash
# Build dist/LamaxTool.app (+ dist/LamaxTool-macOS-<arch>.zip) with PyInstaller on a Mac.
# Needs a python3 with tkinter (the python.org installer or GitHub's setup-python have it;
# Homebrew: brew install python-tk). PyInstaller cannot cross-compile: build on an
# Apple Silicon Mac for arm64 (the GitHub workflow .github/workflows/build-macos.yml does this).
# The camera programs must be built first (bash build.sh, on Linux/WSL) so that
# sdcard/cam/bin is filled.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
[ -f sdcard/cam/bin/camd ] || { echo "sdcard/cam/bin is empty - build the camera programs first (see docs/BUILD.md)"; exit 1; }
python3 -c 'import tkinter' 2>/dev/null || { echo "python3 has no tkinter (brew install python-tk, or use the python.org installer)"; exit 1; }
ARCH=$(uname -m)
VENV=${VENV:-$HOME/.cache/lamaxtool-venv}
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --quiet --upgrade pip pyinstaller sounddevice paramiko
# own output folder: dist/LamaxTool is the committed Linux binary
OUT=build/macos-dist
rm -rf "$OUT" dist/LamaxTool.app
"$VENV/bin/python" -m PyInstaller --noconfirm --clean --onedir --windowed --name LamaxTool --exclude-module numpy \
    --target-arch "$ARCH" --osx-bundle-identifier io.github.lamax-x31-cctv.lamaxtool \
    --distpath "$OUT" --workpath build/pyinstaller-macos --specpath build \
    --add-data "$ROOT/boot/make_boot.py:boot" \
    --add-data "$ROOT/boot/init.camsd.sh:boot" \
    --add-data "$ROOT/sdcard/cam:sdcard/cam" \
    --add-data "$ROOT/ORIGINAL_FW/mtdblock1_boot.bin:ORIGINAL_FW" \
    --add-data "$ROOT/ORIGINAL_FW/SHA256SUMS:ORIGINAL_FW" \
    tools/lamax_tool.py
mkdir -p dist
mv "$OUT/LamaxTool.app" dist/      # the .app bundle is the result (not the --onedir folder)
# zip with ditto so the bundle's symlinks, permissions and ad-hoc signature survive
rm -f "dist/LamaxTool-macOS-$ARCH.zip"
ditto -c -k --keepParent dist/LamaxTool.app "dist/LamaxTool-macOS-$ARCH.zip"
echo "Built: $ROOT/dist/LamaxTool.app and dist/LamaxTool-macOS-$ARCH.zip"
