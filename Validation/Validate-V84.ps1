#Requires -Version 5.1
param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Validate-V8.ps1') -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { throw "V8.4 validation failed: $LASTEXITCODE" }
Write-Host 'V8.4 build/test harness completed; run separate NTFS crash and network/HDD tests.'
