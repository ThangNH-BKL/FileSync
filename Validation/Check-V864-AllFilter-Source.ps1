#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\FileServer\CFServerDlg.cpp') -Raw -Encoding UTF8
$filter = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\Common\FilterSelection.h') -Raw -Encoding UTF8
$begin = $source.IndexOf('void CFServerDlg::UpdateFilterControls()')
$end = $source.IndexOf('bool CFServerDlg::BuildSelectedFilter(', [Math]::Max(0, $begin))
if ($begin -lt 0 -or $end -le $begin) { throw 'ALL handler not found' }
$handler = $source.Substring($begin, $end-$begin)
$checks = @(
    @{ Name='All filter stays visible'; OK=$handler.Contains('control->ShowWindow(SW_SHOW)') -and $handler.Contains('edit->ShowWindow(SW_SHOW)') },
    @{ Name='All disables extension controls'; OK=$handler.Contains('control->EnableWindow(controlState.extensionOptionsEnabled)') },
    @{ Name='All disables custom editor'; OK=$handler.Contains('edit->EnableWindow(controlState.customEditorEnabled)') },
    @{ Name='All does not clear previous checkboxes'; OK=(!$handler.Contains('for (const UINT id : ids) CheckDlgButton(id, BST_UNCHECKED)')) },
    @{ Name='FilterLastSelection saved and restored'; OK=$source.Contains('FilterLastSelection') -and $source.Contains('DisplaySelection(') },
    @{ Name='Portable policy compiled into UI'; OK=$filter.Contains('GetControlState(') },
    @{ Name='Server V8.6.4 window title'; OK=$source.Contains('FileServer V8.6.4 - File Sync') }
)
$failed=0
foreach ($item in $checks) { if ($item.OK) { Write-Host ('PASS: '+$item.Name) } else { Write-Host ('FAIL: '+$item.Name) -ForegroundColor Red; $failed++ } }
if ($failed) { throw "Source checks failed: $failed" }
Write-Host 'Source checks PASS. Build and UI verification must still run on Windows.'
