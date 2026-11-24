#Requires -Version 5.1
param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Validate-V8.ps1') -Configuration $Configuration
if($LASTEXITCODE -ne 0) { throw "V8.5 tests failed: $LASTEXITCODE" }
Write-Host 'V8.5 MSVC tests PASS. Still requires isolated NTFS crash, multi-client, HDD and power-loss validation.'
