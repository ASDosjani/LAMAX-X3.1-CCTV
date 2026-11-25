# Build dist\LamaxTool.exe (single file, no console) with PyInstaller.
# Needs Python 3.9+ on PATH. The camera programs must be built first
# (bash build.sh in WSL) so that sdcard\cam\bin is filled.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path 'sdcard\cam\bin\camd')) { throw 'sdcard\cam\bin is empty - run build.sh first (see docs/BUILD.md)' }
# build venv in a short path (deep repo paths can break pip on Windows)
$venv = Join-Path $env:TEMP 'lamaxtool-venv'
if (-not (Test-Path "$venv\Scripts\python.exe")) { python -m venv $venv }
# PyInstaller logs to stderr; Windows PowerShell 5 would treat that as an error
$ErrorActionPreference = 'Continue'
& "$venv\Scripts\python.exe" -m pip install --quiet --upgrade pip pyinstaller
& "$venv\Scripts\pyinstaller.exe" --noconfirm --clean --onefile --windowed --name LamaxTool `
    --distpath dist --workpath build\pyinstaller --specpath build `
    --add-data "$root\boot\make_boot.py;boot" `
    --add-data "$root\boot\init.camsd.sh;boot" `
    --add-data "$root\sdcard\cam;sdcard\cam" `
    tools\lamax_tool.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed ($LASTEXITCODE)" }
Write-Output "Built: $root\dist\LamaxTool.exe"
