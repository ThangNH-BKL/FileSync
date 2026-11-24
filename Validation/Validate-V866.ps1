#Requires -Version 5.1
param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Check-V866-ServerDialog-Source.ps1')
if($LASTEXITCODE -ne 0) {throw 'V8.6.6 dialog source checks failed'}
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Validate-V865.ps1') -Configuration $Configuration
if($LASTEXITCODE -ne 0){throw 'V8.6.5 inherited tests failed'}
$work=Join-Path $PSScriptRoot ('_build_v866_'+$Configuration)
New-Item -ItemType Directory -Force -Path $work | Out-Null
Push-Location $work
try {
 & cl.exe /nologo /std:c++17 /EHsc /W4 /WX ('/Fe:PortableServerDialogLayout.exe') (Join-Path $PSScriptRoot 'PortableServerDialogLayout.cpp')
 if($LASTEXITCODE -ne 0){throw 'PortableServerDialogLayout compile failed'}
 & (Join-Path $work 'PortableServerDialogLayout.exe')
 if($LASTEXITCODE -ne 0){throw 'PortableServerDialogLayout test failed'}
} finally {Pop-Location}
Write-Host 'PASS: V8.6.6 build and layout tests. Verify visually at Windows 1366x768 / 125%.'
