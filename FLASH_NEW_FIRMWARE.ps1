param([string]$Port = 'COM8', [int]$Baud = 460800)
$ErrorActionPreference = 'Stop'
$juliaBuild = 'D:\CodexData\JuliaPrivate\esp-28848591972c\build'
$juliaPython = 'D:\Espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
$juliaImage = Join-Path $juliaBuild 'julia_fused_base.bin'
if (-not (Test-Path -LiteralPath $juliaImage)) { throw 'Provisioned firmware is missing. Do not substitute the old build directory.' }
Write-Host 'Target: esp-28848591972c; firmware: 0.1.4 device-provisioned control-v1'
Get-FileHash -LiteralPath $juliaImage -Algorithm SHA256 | Format-List

# This script flashes only when the user explicitly runs it. Verify the physical
# eFuse MAC before writing a device-specific image; do not erase NVS or eFuses.
$juliaProbe = & $juliaPython -m esptool --chip esp32s3 --port $Port --baud $Baud --before default_reset --after no_reset read_mac 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify device on the selected port.' }
$juliaMac = [regex]::Match(($juliaProbe -join "`n"), '(?im)MAC:\s*([0-9a-f:]{17})').Groups[1].Value
if ($juliaMac.ToLowerInvariant() -ne '28:84:85:91:97:2c') { throw "Device MAC does not match esp-28848591972c; no firmware was written. Selected MAC: $juliaMac" }
Push-Location -LiteralPath $juliaBuild
try {
    & $juliaPython -m esptool --chip esp32s3 --port $Port --baud $Baud --before no_reset --after hard_reset write_flash '@flash_args'
    $juliaResult = $LASTEXITCODE
} finally { Pop-Location }
exit $juliaResult
