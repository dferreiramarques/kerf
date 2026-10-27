# Builds the Release VST3, the installer and the raw .vst3 zip into ../downloads.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File plugin\release.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
$version = ([regex]::Match((Get-Content "$here\installer\KerfLite.iss" -Raw), 'MyAppVersion "([^"]+)"')).Groups[1].Value

if (-not (Test-Path $iscc)) { throw "Inno Setup not found. Install it: winget install --id JRSoftware.InnoSetup -e" }

Set-Location $here
& $cmake -B build -G "Visual Studio 18 2026" -A x64
if ($LASTEXITCODE) { throw 'configure failed' }
& $cmake --build build --config Release --target Kerf_VST3
if ($LASTEXITCODE) { throw 'build failed' }

& $iscc "$here\installer\KerfLite.iss"
if ($LASTEXITCODE) { throw 'installer failed' }

$vst3 = "$here\build\Kerf_artefacts\Release\VST3\Kerf Lite (Beta).vst3"
$zip = "$here\..\downloads\KerfLite_${version}_Beta_VST3_x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $vst3 -DestinationPath $zip

Get-ChildItem "$here\..\downloads\KerfLite_${version}_*" | Select-Object Name, Length
