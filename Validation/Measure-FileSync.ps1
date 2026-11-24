#Requires -Version 5.1
<#
Samples a running FileClient and FileServer without editing their files.
CPU% normalized to total machine logical processors; IO counters include
process file/device IO and are NOT guaranteed physical disk throughput.
#>
param(
    [Parameter(Mandatory=$true)][int]$ClientPid,
    [Parameter(Mandatory=$true)][int]$ServerPid,
    [ValidateRange(5,7200)][int]$Seconds=120,
    [ValidateRange(1,30)][int]$IntervalSeconds=2,
    [string]$OutputCsv=".\FileSync_Perf_Samples.csv"
)
$ErrorActionPreference='Stop'
$native=@'
using System;
using System.Runtime.InteropServices;
public static class FileSyncPerfIo {
  [StructLayout(LayoutKind.Sequential)]
  public struct IOCounters {
    public ulong ReadOperationCount,WriteOperationCount,OtherOperationCount;
    public ulong ReadTransferCount,WriteTransferCount,OtherTransferCount;
  }
  [DllImport("kernel32.dll",SetLastError=true)]
  public static extern bool GetProcessIoCounters(IntPtr handle, out IOCounters io);
}
'@
if(-not ([System.Management.Automation.PSTypeName]'FileSyncPerfIo').Type) {Add-Type -TypeDefinition $native -ErrorAction Stop}
$last=@{}
$logical=[Environment]::ProcessorCount
$end=(Get-Date).AddSeconds($Seconds)
$first=$true
if(Test-Path -LiteralPath $OutputCsv) {throw "Output exists; supply a new CSV filename: $OutputCsv"}
while((Get-Date) -lt $end) {
  foreach($entry in @(@{label='client';pid=$ClientPid},@{label='server';pid=$ServerPid})) {
    try { $p=Get-Process -Id $entry.pid -ErrorAction Stop } catch {Write-Warning "$($entry.label) PID $($entry.pid) not running";continue}
    $now=Get-Date
    $io=New-Object FileSyncPerfIo+IOCounters
    try {$gotIo=[FileSyncPerfIo]::GetProcessIoCounters($p.Handle,[ref]$io)} catch {$gotIo=$false}
    $key="$($entry.label):$($entry.pid)"
    $cpuPct=0.0;$readRate=0.0;$writeRate=0.0
    if($last.ContainsKey($key)) {
      $prev=$last[$key];$dt=($now-$prev.t).TotalSeconds
      if($dt -gt 0) {
        $cpuPct=[math]::Max(0,100.0*(($p.CPU - $prev.cpu)/$dt)/$logical)
        if($gotIo -and $prev.io) {
          $readRate=[math]::Max(0,([double]$io.ReadTransferCount-$prev.read)/$dt/1MB)
          $writeRate=[math]::Max(0,([double]$io.WriteTransferCount-$prev.write)/$dt/1MB)
        }
      }
    }
    $last[$key]=@{t=$now;cpu=$p.CPU;io=$gotIo;read=[double]$io.ReadTransferCount;write=[double]$io.WriteTransferCount}
    $r=[pscustomobject]@{
      UTC=$now.ToUniversalTime().ToString('o');Role=$entry.label;PID=$entry.pid
      CpuPctOfMachine=[math]::Round($cpuPct,2)
      WorkingSetMiB=[math]::Round($p.WorkingSet64/1MB,2)
      PrivateMiB=[math]::Round($p.PrivateMemorySize64/1MB,2)
      ThreadCount=$p.Threads.Count;HandleCount=$p.HandleCount
      ProcessIoReadMiBps=if($gotIo){[math]::Round($readRate,3)}else{-1}
      ProcessIoWriteMiBps=if($gotIo){[math]::Round($writeRate,3)}else{-1}
    }
    $r|Export-Csv -LiteralPath $OutputCsv -NoTypeInformation -Append -Encoding UTF8
    if($first){Write-Host "Writing CSV $OutputCsv";$first=$false}
  }
  Start-Sleep -Seconds $IntervalSeconds
}
Write-Host "Measurements finished: $OutputCsv"
