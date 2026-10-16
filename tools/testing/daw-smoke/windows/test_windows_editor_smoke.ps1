# Fixture-only controls; never starts REAPER or sends global input.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'windows_editor_common.ps1')
$root=Join-Path ([IO.Path]::GetTempPath()) ('pulp-windows-harness-controls-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$checks=0
function Check([bool]$Value,[string]$Name){if(-not $Value){throw "FAIL: $Name"};$script:checks++;Write-Output "PASS: $Name"}
function Reject([scriptblock]$Action,[string]$Name){$rejected=$false;try{& $Action}catch{$rejected=$true};Check $rejected $Name}
try {
  foreach($f in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1'){
    $tokens=$null;$errors=$null
    [System.Management.Automation.Language.Parser]::ParseFile($f.FullName,[ref]$tokens,[ref]$errors)|Out-Null
    Check ($errors.Count -eq 0) ('PowerShell parser '+$f.Name)
  }
  foreach($fixture in @(@(0x8664,'x64','vstpath64'),@(0xaa64,'arm64','vstpath_winarm64'),@(0xa641,'arm64ec','vstpath_winarm64'),@(0xa64e,'arm64x','vstpath_winarm64'))){
    $path=Join-Path $root ($fixture[1]+'.exe')
    $bytes=New-Object byte[] 128
    [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($bytes,0)
    [BitConverter]::GetBytes([uint32]64).CopyTo($bytes,60)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes,64)
    [BitConverter]::GetBytes([uint16]$fixture[0]).CopyTo($bytes,68)
    [IO.File]::WriteAllBytes($path,$bytes)
    $machine=Get-PeMachine $path;$keys=Get-ReaperScanKeys $machine
    Check ($machine -eq $fixture[1] -and $keys[0] -eq $fixture[2] -and $keys[1] -eq 'clap_path') ('PE architecture/scan keys '+$fixture[1])
  }
  [IO.File]::WriteAllBytes((Join-Path $root 'bad.exe'),[byte[]]@(0,0))
  Reject {Get-PeMachine (Join-Path $root 'bad.exe')} 'invalid PE refused'
  Check ((Join-WindowsArguments @('-cfgfile','C:\Space Folder\reaper.ini','C:\script path\start.lua')) -eq '"-cfgfile" "C:\Space Folder\reaper.ini" "C:\script path\start.lua"') 'paths with spaces preserve argument boundaries'
  Reject {Join-WindowsArguments @("bad`nargument")} 'newline argument refused'
  $state=Join-Path $root 'state.rpp';'fixture'|Set-Content -LiteralPath $state
  $good=@{phase='AUTOMATION_DONE';fx_shown='1';param_index='0';param_count='1';observations='20';observed_changes='19';max_error='0';state_saved=$state}
  Assert-TerminalStatus $good 0;Check $true 'terminal retains insertion and observed parameter proof'
  $bad=$good.Clone();$bad.Remove('fx_shown')
  Reject {Assert-TerminalStatus $bad 0} 'old overwrite-style terminal without insertion refused'
  $bad=$good.Clone();$bad.phase='AUTOMATION_RUNNING'
  Reject {Assert-TerminalStatus $bad 0} 'premature running marker refused'
  $bad=$good.Clone();$bad.observed_changes='0'
  Reject {Assert-TerminalStatus $bad 0} 'requested-only unchanged readback refused'
  $bad=$good.Clone();$bad.max_error='NaN'
  Reject {Assert-TerminalStatus $bad 0} 'nonfinite readback error refused'
  $bad=$good.Clone();$bad.state_saved=Join-Path $root 'missing.rpp'
  Reject {Assert-TerminalStatus $bad 0} 'missing saved state refused'
  $editor=@{phase='EDITOR_READY';fx_shown='1';state_saved=$state}
  Assert-TerminalStatus $editor -1;Check $true 'no-automation terminal keeps editor handshake'
  $status=Join-Path $root 'status.txt';@('token=old','phase=EDITOR_READY')|Set-Content -LiteralPath $status
  Reject {Read-HostStatus $status 'new'} 'stale run token refused'
  $manifest=@(@{path='C:\isolated\Spectr.vst3';sha256='abc'})
  Assert-ModuleBinding 'C:\isolated\Spectr.vst3' 'ABC' $manifest
  Check $true 'exact module path/hash accepted'
  Reject {Assert-ModuleBinding 'C:\system\Spectr.vst3' 'abc' $manifest} 'same-name system plugin refused'
  Reject {Assert-ModuleBinding 'C:\isolated\Spectr.vst3' 'def' $manifest} 'changed loaded PE hash refused'
  Assert-EditorEvidence 1234 $true $true $true $true
  Check $true 'visible owned editor binding accepted'
  Reject {Assert-EditorEvidence 0 $false $false $true $true} 'missing editor refused'
  Reject {Assert-EditorEvidence 1234 $true $false $true $true} 'hidden editor refused'
  Reject {Assert-EditorEvidence 1234 $true $true $true $false} 'main-window-only foreground refused'
  Reject {Assert-EditorEvidence 1234 $true $true $false $true} 'foreign editor refused'
  $seed=Join-Path $root 'source profile';$private=Join-Path $root 'private profile';$export=Join-Path $root 'export'
  New-Item -ItemType Directory -Path $seed,$export | Out-Null
  '[REAPER]'|Set-Content -LiteralPath (Join-Path $seed 'reaper.ini')
  'fixture private registration'|Set-Content -LiteralPath (Join-Path $seed 'reaper-license.rk')
  $sourceHash=(Get-FileHash -LiteralPath (Join-Path $seed 'reaper-license.rk')).Hash
  Copy-PrivateProfile $seed $private $export
  Check (Test-Path -LiteralPath (Join-Path $private 'reaper-license.rk')) 'copied registration is private'
  Check (@(Get-ChildItem -LiteralPath $export -Recurse -Filter '*license*').Count -eq 0) 'export evidence excludes copied registration'
  Reject {Copy-PrivateProfile $seed (Join-Path $export 'bad-profile') $export} 'profile inside export is refused'
  Remove-Item -LiteralPath $private -Recurse -Force
  Check ((Get-FileHash -LiteralPath (Join-Path $seed 'reaper-license.rk')).Hash -eq $sourceHash) 'private cleanup preserves source registration'
  # Execute the real wrapper's early-failure path; require a durable receipt.
  $evidence=Join-Path $root 'failure evidence'
  $engine=(Get-Process -Id $PID).Path
  & $engine -NoProfile -File (Join-Path $PSScriptRoot 'windows_editor_smoke.ps1') -ReaperExe (Join-Path $root 'missing.exe') -Format vst3 -PluginPath (Join-Path $root 'missing.vst3') -PluginName Example -SeedProfile $root -EvidenceDir $evidence | Out-Null
  $code=$LASTEXITCODE
  $receipts=@(Get-ChildItem -LiteralPath $evidence -Filter receipt.json -Recurse)
  Check ($code -eq 3 -and $receipts.Count -eq 1) 'early missing-host failure leaves terminal receipt'
  $receipt=Get-Content -LiteralPath $receipts[0].FullName -Raw|ConvertFrom-Json
  Check ($receipt.verdict -eq 'INCONCLUSIVE' -and $receipt.error -match 'REAPER executable missing') 'failure receipt carries typed reason'
  Write-Output "PASS: $checks fixture checks; product runtime NOT EVALUATED"
} finally {
  Remove-Item -LiteralPath $root -Recurse -Force
}
