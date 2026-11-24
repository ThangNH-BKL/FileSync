#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..'
$dlg = Get-Content -LiteralPath (Join-Path $root 'FileServer\CFServerDlg.cpp') -Raw -Encoding UTF8
$rc = Get-Content -LiteralPath (Join-Path $root 'FileServer\FileServer.rc') -Raw -Encoding UTF8
$ids = Get-Content -LiteralPath (Join-Path $root 'FileServer\resource.h') -Raw -Encoding UTF8
$engine = Get-Content -LiteralPath (Join-Path $root 'Common\ServerEngine.cpp') -Raw -Encoding UTF8
$config = Get-Content -LiteralPath (Join-Path $root 'Common\ServerEngine.h') -Raw -Encoding UTF8
$checks = @(
 @{Name='MaxClients edit control in dialog';OK=($rc -match 'EDITTEXT\s+IDC_EDIT_MAX_CLIENTS')},
 @{Name='ActiveTransfers edit control in dialog';OK=($rc -match 'EDITTEXT\s+IDC_EDIT_ACTIVE_TRANSFERS')},
 @{Name='Unique numeric IDs';OK=($ids.Contains('IDC_EDIT_MAX_CLIENTS') -and $ids.Contains('IDC_EDIT_ACTIVE_TRANSFERS'))},
 @{Name='UI loads and stores values';OK=($dlg.Contains('L"MaxClients"') -and $dlg.Contains('L"MaxActiveTransfers"'))},
 @{Name='Strict UI validation';OK=($dlg.Contains('ui_server_limits::Parse(') -and $dlg.Contains('Invalid server limits'))},
 @{Name='UI passes max clients to ServerEngine';OK=($dlg.Contains('cfg.maxClients = maxClients;'))},
 @{Name='UI passes active transfers to ServerEngine';OK=($dlg.Contains('cfg.maxActiveTransfers = maxTransfers;'))},
 @{Name='UI values override legacy environment';OK=($dlg.Contains('cfg.limitsFromDialog = true') -and $config.Contains('limitsFromDialog = false') -and ($engine -match 'if\s*\(!cfg_\.limitsFromDialog\s*&&'))},
 @{Name='Editable only when Server stopped';OK=($dlg.Contains('IDC_EDIT_MAX_CLIENTS)->EnableWindow(enabled)') -and $dlg.Contains('IDC_EDIT_ACTIVE_TRANSFERS)->EnableWindow(enabled)'))},
 @{Name='New version label';OK=($dlg.Contains('FileServer V8.6.5 - File Sync') -or $dlg.Contains('FileServer V8.6.6 - File Sync'))}
)
$failed=0
foreach ($c in $checks) { if($c.OK){ Write-Host ('PASS: '+$c.Name) } else { Write-Host ('FAIL: '+$c.Name) -ForegroundColor Red; $failed++ } }
if($failed){ throw "$failed V8.6.5 source checks failed" }
Write-Host 'PASS: V8.6.5 source checks. Windows MFC integration must still be tested.'
