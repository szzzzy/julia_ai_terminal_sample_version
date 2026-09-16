# Explicit experiment flash entry. Never uses the product build/ directory.
param([string]$Port = 'COM8', [int]$Baud = 460800, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$juliaProject = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$juliaBuild = Join-Path $juliaProject 'build-imu-test'
$juliaConfigPath = Join-Path $juliaBuild 'config/sdkconfig.json'
$juliaImage = Join-Path $juliaBuild 'julia_fused_base.bin'
$juliaPython = 'D:\Espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $juliaConfigPath) -or -not (Test-Path -LiteralPath $juliaImage)) {
    throw 'Build the independent IMU test firmware first. See tools/imu_logger/README.md.'
}
$juliaConfig = Get-Content -LiteralPath $juliaConfigPath -Raw | ConvertFrom-Json
if ($juliaConfig.JULIA_IMU_LOGGER_ENABLE -ne $true) {
    throw 'Refusing to flash: build-imu-test is not configured as IMU experiment firmware.'
}
foreach ($juliaFile in @('flash_args', 'bootloader/bootloader.bin', 'partition_table/partition-table.bin', 'ota_data_initial.bin')) {
    if (-not (Test-Path -LiteralPath (Join-Path $juliaBuild $juliaFile))) {
        throw "Missing experiment build output: $juliaFile"
    }
}
Write-Host "IMU EXPERIMENT firmware: $juliaImage"
Write-Host 'Target: esp-28848591972c. Product voice/FSM will not run in this image.'
Get-FileHash -LiteralPath $juliaImage -Algorithm SHA256 | Format-List
if ($CheckOnly) { Write-Host 'Checks passed; no serial connection or flash performed.'; exit 0 }

$juliaProbe = & $juliaPython -m esptool --chip esp32s3 --port $Port --baud $Baud --before default_reset --after no_reset read_mac 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify device on the selected port.' }
$juliaMac = [regex]::Match(($juliaProbe -join "`n"), '(?im)MAC:\s*([0-9a-f:]{17})').Groups[1].Value
if ($juliaMac.ToLowerInvariant() -ne '28:84:85:91:97:2c') {
    throw "Device MAC mismatch; no firmware was written. Selected MAC: $juliaMac"
}
Push-Location -LiteralPath $juliaBuild
try {
    & $juliaPython -m esptool --chip esp32s3 --port $Port --baud $Baud --before no_reset --after hard_reset write_flash '@flash_args'
    $juliaResult = $LASTEXITCODE
} finally { Pop-Location }
exit $juliaResult
