#Requires -Version 5.1
param([Parameter(Mandatory=$true)][string]$ServerLog)
$ErrorActionPreference='Stop'
$matches=Select-String -LiteralPath $ServerLog -Pattern 'PERF v85_group_wal records=(\d+) wal_flush_count=(\d+) wal_flush_us=(\d+) failed_batches=(\d+)'
foreach($m in $matches){
  $v=$m.Matches[0].Groups
  $records=[double]$v[1].Value
  $flushes=[double]$v[2].Value
  [pscustomobject]@{
    Records=[long]$records
    FlushCount=[long]$flushes
    RecordsPerFlush=if($flushes -gt 0){[math]::Round($records/$flushes,2)}else{0}
    FlushTimeMs=[math]::Round(([double]$v[3].Value)/1000,2)
    FailedBatches=[long]$v[4].Value
  }
}
