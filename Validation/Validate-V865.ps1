#Requires -Version 5.1
param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Check-V865-ServerLimits-Source.ps1')
if($LASTEXITCODE -ne 0){ throw 'Source checks failed' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Validate-V85.ps1') -Configuration $Configuration
if($LASTEXITCODE -ne 0){ throw 'Legacy MSVC tests failed' }
# Compile new portable parser test using Visual Studio Developer Command Prompt environment.
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if(-not $cl){ throw 'cl.exe not available. Run from Developer PowerShell for VS 2022.' }
$work = Join-Path $PSScriptRoot ('_build_v865_'+$Configuration)
New-Item -ItemType Directory -Force -Path $work | Out-Null
Push-Location $work
try {
  & cl.exe /nologo /std:c++17 /EHsc /W4 /WX ('/Fe:PortableServerLimits.exe') (Join-Path $PSScriptRoot 'PortableServerLimits.cpp')
  if($LASTEXITCODE -ne 0){ throw 'PortableServerLimits compile failed' }
  & (Join-Path $work 'PortableServerLimits.exe')
  if($LASTEXITCODE -ne 0){ throw 'PortableServerLimits test failed' }
} finally { Pop-Location }
Write-Host 'PASS: V8.6.5 source, legacy MSVC and server limits tests.'
Write-Host 'Still requires actual MFC UI and 100-300 Client stress testing.'
