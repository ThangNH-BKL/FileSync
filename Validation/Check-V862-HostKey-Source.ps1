#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\FileServer\CFServerDlg.cpp') -Raw -Encoding UTF8
$header = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\FileServer\CFServerDlg.h') -Raw -Encoding UTF8
$helper = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\Common\UiNetworkPath.h') -Raw -Encoding UTF8
$checks = @(
  @{ Name='Normal D share generated'; Ok=($helper.Contains('StandardSharePath') -and $helper.Contains('CandidateSharePaths')) },
  @{ Name='Normal share attempted before admin fallback'; Ok=($source.Contains('const auto paths = fsyn::ui_path::CandidateSharePaths') -and $source.Contains('paths.front()')) },
  @{ Name='Do not fall back on access denied'; Ok=($source.Contains('error != ERROR_BAD_NET_NAME') -and $source.Contains('error != ERROR_BAD_NETPATH') -and $source.Contains('error != ERROR_PATH_NOT_FOUND')) },
  @{ Name='Network folder validated before open'; Ok=($source.Contains('::GetFileAttributesW(path.c_str())')) },
  @{ Name='Direct UNC shell target'; Ok=($source.Contains('launch.lpFile = unc.c_str();') -and $source.Contains('::ShellExecuteExW(&launch)')) },
  @{ Name='Errors list network paths tried'; Ok=($source.Contains('for (const auto& attempt : attempts)') -and $header.Contains('std::vector<std::pair<std::wstring, DWORD>> attempts;')) },
  @{ Name='HostKey and Tree have separate local/remote handlers'; Ok=($source.Contains('void CFServerDlg::OnNmDblClickTree(') -and $source.Contains('BuildServerRelativePath(') -and $source.Contains('OpenClientDataFolder(')) },
  @{ Name='Build title updated'; Ok=$source.Contains('FileServer V8.6.3 - File Sync') }
)
$failed = 0
foreach ($check in $checks) {
  if ($check.Ok) { Write-Host ('PASS: ' + $check.Name) -ForegroundColor Green }
  else { Write-Host ('FAIL: ' + $check.Name) -ForegroundColor Red; $failed++ }
}
if ($failed -gt 0) { throw "V8.6.2 source check failed: $failed" }
Write-Host 'Source checks passed. This is not an MSVC build or SMB connectivity test.'
