#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\FileServer\CFServerDlg.cpp') -Raw -Encoding UTF8
$helper = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\Common\UiServerTreePath.h') -Raw -Encoding UTF8
$clientStart = $source.IndexOf('void CFServerDlg::OnNmDblClickClients(')
$treeStart = $source.IndexOf('void CFServerDlg::OnNmDblClickTree(')
$treeEnd = $source.IndexOf('void CFServerDlg::OnCancel(', [Math]::Max(0, $treeStart))
if ($clientStart -lt 0 -or $treeStart -lt 0 -or $treeEnd -lt 0) { throw 'Cannot find MFC handlers' }
$clientHandler = $source.Substring($clientStart, $treeStart - $clientStart)
$treeHandler = $source.Substring($treeStart, $treeEnd - $treeStart)
$filterStart = $source.IndexOf('void CFServerDlg::UpdateFilterControls(')
$filterEnd = $source.IndexOf('void CFServerDlg::OnBnClickedFilterOption(', [Math]::Max(0, $filterStart))
if ($filterStart -lt 0 -or $filterEnd -lt 0) { throw 'Cannot find filter handler' }
$filterHandler = $source.Substring($filterStart, $filterEnd - $filterStart)
$checks = @(
    @{ Name='HostKey retains Client SMB navigation'; Ok=$clientHandler.Contains('OpenClientDataFolder(') },
    @{ Name='Tree no longer opens Client SMB'; Ok=(!$treeHandler.Contains('OpenClientDataFolder(') -and !$treeHandler.Contains('CandidateSharePaths')) },
    @{ Name='Tree resolves local ServerRoot plus tree path'; Ok=($treeHandler.Contains('BuildServerRelativePath(') -and $treeHandler.Contains('PathSanitizer::Validate(')) },
    @{ Name='Tree validates folder before ShellExecute'; Ok=($treeHandler.Contains('::GetFileAttributesW(') -and $treeHandler.Contains('::ShellExecuteExW(')) },
    @{ Name='Tree rejects unsafe components'; Ok=($helper.Contains('IsSafeComponent(') -and $helper.Contains('BuildServerRelativePath(')) },
    @{ Name='ALL disables but shows extension options'; Ok=($filterHandler.Contains('control->ShowWindow(SW_SHOW)') -and $filterHandler.Contains('control->EnableWindow(controlState.extensionOptionsEnabled)') -and $filterHandler.Contains('edit->EnableWindow(controlState.customEditorEnabled)')) },
    @{ Name='Build title identifies V8.6.4'; Ok=$source.Contains('FileServer V8.6.4 - File Sync') }
)
$failed = 0
foreach ($check in $checks) {
    if ($check.Ok) { Write-Host ('PASS: ' + $check.Name) -ForegroundColor Green }
    else { Write-Host ('FAIL: ' + $check.Name) -ForegroundColor Red; $failed++ }
}
if ($failed -gt 0) { throw "Source checks failed: $failed" }
Write-Host 'Source checks PASS. MSVC/MFC and Explorer behavior still require Windows verification.'
