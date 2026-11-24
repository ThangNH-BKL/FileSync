#Requires -Version 5.1
# FileSync V8 Windows build/test harness. Does not start Server or Client.
param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$sln=Join-Path $root 'FileSync.sln'
$msbuild=Get-Command msbuild.exe -ErrorAction SilentlyContinue
if(-not $msbuild) { throw 'msbuild.exe unavailable. Start VS2022 Developer PowerShell with C++ MFC/SDK workload.' }
Write-Host "Build FileSync.sln | $Configuration | x64"
& $msbuild.Source $sln '/t:Rebuild' "/p:Configuration=$Configuration" '/p:Platform=x64' '/m:2' '/v:minimal'
if($LASTEXITCODE -ne 0) { throw "MSBuild FAILED: $LASTEXITCODE" }
$tests=Join-Path $root "build\x64\$Configuration\Tests.exe"
if(-not (Test-Path $tests)) { throw "Tests.exe not found at $tests - inspect MSBuild OutDir" }
& $tests
if($LASTEXITCODE -eq -1073741819) { throw "Tests.exe access violation (0xC0000005). Review the last [RUN] test and its prerequisites; do not deploy." }
if($LASTEXITCODE -ne 0) { throw "Tests FAILED: $LASTEXITCODE" }
Write-Host 'Unit tests PASS on this machine. This does NOT certify production or IOCP/USN E2E behavior.'
