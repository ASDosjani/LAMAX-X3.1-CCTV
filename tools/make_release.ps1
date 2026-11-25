# Collect the release files into dist\release-<version>\:
#   LamaxTool.exe, LamaxTool (Linux, if built), sdcard.zip (the cam folder), SHA256SUMS.txt
#   .\tools\make_release.ps1 -Version 1.0.0
param([Parameter(Mandatory)] [string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$need = 'camd', 'camui', 'camled', 'camnet', 'wpa_supplicant', 'wpa_cli'
foreach ($b in $need) { if (-not (Test-Path "sdcard\cam\bin\$b")) { throw "sdcard\cam\bin\$b missing - run build.sh and third_party/build_wpa.sh" } }

$out = Join-Path $root "dist\release-$Version"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory $out | Out-Null

# sdcard.zip: cam/autorun.sh, cam/config.txt (template), cam/bin/*
# Built with System.IO.Compression so entry names use '/' (Compress-Archive in
# Windows PowerShell 5 writes '\', which breaks unzipping on Linux / macOS).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$files = [ordered]@{ 'cam/autorun.sh' = 'sdcard\cam\autorun.sh'; 'cam/config.txt' = 'sdcard\cam\config.txt' }
foreach ($b in $need) { $files["cam/bin/$b"] = "sdcard\cam\bin\$b" }
$zip = [IO.Compression.ZipFile]::Open("$out\sdcard.zip", [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($e in $files.GetEnumerator()) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $root $e.Value), $e.Key,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }

foreach ($f in 'dist\LamaxTool.exe', 'dist\LamaxTool') {
    if (Test-Path $f) { Copy-Item $f $out } else { Write-Warning "$f not built - skipped" }
}
Get-ChildItem $out -File | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii "$out\SHA256SUMS.txt"
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
Write-Output "Release files: $out"
