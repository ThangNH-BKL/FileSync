#Requires -Version 5.1
$ErrorActionPreference='Stop'
$root=Join-Path $PSScriptRoot '..'
$rc=Get-Content (Join-Path $root 'FileServer\FileServer.rc') -Raw -Encoding UTF8
$dlg=Get-Content (Join-Path $root 'FileServer\CFServerDlg.cpp') -Raw -Encoding UTF8
$hdr=Get-Content (Join-Path $root 'FileServer\CFServerDlg.h') -Raw -Encoding UTF8
$layout=Get-Content (Join-Path $root 'FileServer\ServerDialogLayout.h') -Raw -Encoding UTF8
$checks=@(
 @{Label='Compact 535x310 dialog';Pass=($rc -match 'IDD_FILESERVER_DIALOG\s+DIALOGEX\s+0,\s*0,\s*535,\s*310')},
 @{Label='Resizable window and columns';Pass=($rc.Contains('WS_THICKFRAME') -and $dlg.Contains('ON_WM_SIZE()') -and $hdr.Contains('OnSize(UINT'))},
 @{Label='Fits monitor working area';Pass=($dlg.Contains('MonitorFromWindow') -and $dlg.Contains('rcWork') -and $dlg.Contains('FitWindowToWorkArea()'))},
 @{Label='Reflows clients, tree and log';Pass=($dlg.Contains('IDC_LIST_CLIENTS, layout.clients') -and $dlg.Contains('IDC_TREE_FILES, layout.tree') -and $dlg.Contains('IDC_LIST_LOG, layout.log'))},
 @{Label='Reflows progress and status';Pass=($dlg.Contains('IDC_PROGRESS_FILES, layout.progress') -and $dlg.Contains('IDC_STATIC_STATUS, layout.status'))},
 @{Label='Server settings not removed';Pass=($rc.Contains('IDC_EDIT_MAX_CLIENTS') -and $rc.Contains('IDC_EDIT_ACTIVE_TRANSFERS'))},
 @{Label='Filter settings preserved';Pass=($rc.Contains('IDC_CHECK_FILTER_ALL') -and $rc.Contains('IDC_CHECK_FILTER_OTHER'))},
 @{Label='V8.6.6 title';Pass=$dlg.Contains('FileServer V8.6.6 - File Sync')}
)
foreach($c in $checks){if(-not $c.Pass){ throw ('FAIL: '+$c.Label) };Write-Host ('PASS: '+$c.Label)}
Write-Host 'Source review PASS. Actual DPI and MSVC UI testing still required.'
