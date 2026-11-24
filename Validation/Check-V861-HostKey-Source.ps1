#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\FileServer\CFServerDlg.cpp') -Raw -Encoding UTF8
$header = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\FileServer\CFServerDlg.h') -Raw -Encoding UTF8
$checks = @(
  @{ Name='No explorer.exe as a launched program'; Ok=($source -notmatch 'ShellExecuteW\([^;]*L"explorer\.exe"') },
  @{ Name='SMB directory is checked before opening'; Ok=($source.Contains('GetFileAttributesW(unc.c_str())')) },
  @{ Name='Actual UNC folder is the shell target'; Ok=($source.Contains('launch.lpFile = unc.c_str();')) },
  @{ Name='Folder is launched by ShellExecuteEx'; Ok=($source.Contains('ShellExecuteExW(&launch)')) },
  @{ Name='Asynchronous SMB probe'; Ok=($source.Contains('std::thread([state, unc]()')) },
  @{ Name='Result dispatched to dialog UI thread'; Ok=($source.Contains('WM_APP_CLIENT_FOLDER_RESULT') -and $source.Contains('OnAppClientFolderResult')) },
  @{ Name='Lifetime state declared'; Ok=($header.Contains('folderProbe_') -and $source.Contains('folderProbe_->alive = false')) }
)
$failed = 0
foreach ($c in $checks) {
  if ($c.Ok) { Write-Host ("PASS: " + $c.Name) -ForegroundColor Green }
  else { Write-Host ("FAIL: " + $c.Name) -ForegroundColor Red; $failed++ }
}
if ($failed) { throw "V8.6.1 source checks failed: $failed" }
Write-Host 'Source checks PASS. This is NOT an MSVC build or live SMB access test.'
