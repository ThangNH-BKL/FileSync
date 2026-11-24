#Requires -Version 5.1
param([Parameter(Mandatory=$true)][string]$ServerRoot)
$ErrorActionPreference = 'Stop'
$root=[IO.Path]::GetFullPath($ServerRoot)
$state=Join-Path $root '.fsync_state\baseline_lsm_v85'
$wal=Join-Path $state 'baseline.wal'
if(-not (Test-Path -LiteralPath $wal -PathType Leaf)) { throw "WAL missing: $wal. No changes made." }
if(Get-Process -Name FileServer -ErrorAction SilentlyContinue) { throw 'FileServer process detected. Stop ALL FileServer instances before recovery.' }
Write-Host "ServerRoot: $root"
Write-Host "WAL preserved: $wal"
Write-Host 'IMPORTANT: create an independent verified backup of the entire .fsync_state before proceeding.' -ForegroundColor Yellow
$confirm=Read-Host 'Type REBUILD exactly to quarantine derived SST/manifest (no WAL deletion)'
if($confirm -cne 'REBUILD') { Write-Host 'Cancelled'; exit 2 }
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$quarantine=Join-Path $state ("quarantine_"+$stamp)
New-Item -ItemType Directory -Path $quarantine -Force | Out-Null
foreach($name in @('manifest.bin','runs')) {
  $src=Join-Path $state $name
  if(Test-Path -LiteralPath $src) {Move-Item -LiteralPath $src -Destination $quarantine -ErrorAction Stop}
}
Write-Host "Quarantined derived index to: $quarantine"
Write-Host 'The WAL was not modified. Run V8.5 LSM mode on a TEST copy to replay; validate identities and CRC before production.'
