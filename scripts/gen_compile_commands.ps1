# Generate the real compile database from this project's CMake source lists.
# Run in an activated ESP-IDF PowerShell; no sibling project is required.
param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$juliaProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $env:IDF_PATH) { throw 'Activate ESP-IDF before running this script.' }
$juliaIdf = Join-Path $env:IDF_PATH 'tools/idf.py'
$juliaPython = if ($env:IDF_PYTHON_ENV_PATH) {
    Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
} else { 'python' }
Push-Location -LiteralPath $juliaProjectRoot
try {
    & $juliaPython $juliaIdf -B $BuildDirectory reconfigure
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF configuration failed.' }
    $juliaDatabase = Join-Path $BuildDirectory 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $juliaDatabase)) {
        throw 'CMake did not generate compile_commands.json.'
    }
    Write-Output "Compile database generated: $juliaDatabase"
} finally { Pop-Location }
