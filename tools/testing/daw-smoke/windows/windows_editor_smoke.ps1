<#!
.SYNOPSIS
  Opt-in Windows host insertion/editor evidence, with exact loaded-PE binding.
  Product interaction, animation and live audio acceptance are separate gates.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$ReaperExe,
  [Parameter(Mandatory=$true)][ValidateSet('vst3','clap')][string]$Format,
  [Parameter(Mandatory=$true)][string]$PluginPath,
  [Parameter(Mandatory=$true)][string]$PluginName,
  [Parameter(Mandatory=$true)][string]$EvidenceDir,
  [Parameter(Mandatory=$true)][string]$SeedProfile,
  [string[]]$SidecarPath = @(),
  [int]$ParamIndex = -1,
  [ValidateRange(1,60)][int]$AutomationSec = 8,
  [ValidateRange(5,300)][int]$TimeoutSec = 60,
  [int]$ContextX = -1,
  [int]$ContextY = -1
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows_editor_common.ps1')
$token = [Guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$run = Join-Path (Resolve-Path -LiteralPath $EvidenceDir).Path ('run-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + $token.Substring(0,8))
New-Item -ItemType Directory -Path $run | Out-Null
$receipt = [ordered]@{schema=2;token=$token;verdict='INCONCLUSIVE';scope='host insertion/editor/optional host parameter readback';started_utc=[DateTime]::UtcNow.ToString('o');error=$null;cleanup_errors=@()}
$owned = New-Object 'System.Collections.Generic.List[object]'
$priorEnv = @{}
$envNames = @('FX','STATUS','TOKEN','PROJECT','PARAM_INDEX','DURATION') | ForEach-Object {'PULP_DAW_SMOKE_'+$_}
foreach($name in $envNames){$priorEnv[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$exitCode=3
$privateProfile=$null
function Stop-Owned($Entry) {
  if ($Entry.process.HasExited) { return }
  Assert-OwnedIdentity $Entry.process $Entry.started
  $Entry.process.Kill(); $Entry.process.WaitForExit(5000) | Out-Null
}
function Start-Owned([string[]]$Arguments,[string]$Label) {
  $command=Join-WindowsArguments $Arguments
  $p=Start-Process -FilePath $script:reaper -ArgumentList $command -WorkingDirectory $run -RedirectStandardOutput (Join-Path $run ($Label+'.stdout.txt')) -RedirectStandardError (Join-Path $run ($Label+'.stderr.txt')) -PassThru
  if ($p.SessionId -ne [int]$receipt.session.session_id) { throw 'Owned REAPER process entered a different Windows session' }
  $entry=@{process=$p;started=$p.StartTime.ToUniversalTime().ToString('o');command=$command;session_id=[int]$p.SessionId}
  $owned.Add($entry); return $entry
}
try {
  if(-not(Test-Path -LiteralPath $ReaperExe -PathType Leaf)){throw "REAPER executable missing: $ReaperExe"}
  $script:reaper=(Resolve-Path -LiteralPath $ReaperExe).Path
  $receipt.session=Get-InteractiveDesktopContext
  $hostPe=Get-PeIdentity $reaper; $keys=Get-ReaperScanKeys $hostPe.execution_abi
  $receipt.host=@{path=$reaper;pe=$hostPe;sha256=(Get-FileHash -LiteralPath $reaper -Algorithm SHA256).Hash}
  if(-not(Test-Path -LiteralPath (Join-Path $SeedProfile 'reaper.ini') -PathType Leaf)){throw 'SeedProfile must contain an initialized reaper.ini'}
  if(-not(Test-Path -LiteralPath $PluginPath)){throw "Plugin missing: $PluginPath"}
  $plugin=(Resolve-Path -LiteralPath $PluginPath).Path
  if([IO.Path]::GetExtension($plugin) -ine ('.'+$Format)){throw 'Plugin format/path mismatch'}
  $privateProfile=Join-Path ([IO.Path]::GetTempPath()) ('pulp-reaper-profile-'+$token)
  $portable=$privateProfile;$scan=Join-Path $run 'scan'
  New-Item -ItemType Directory -Path $scan | Out-Null
  # Sensitive copied settings/license remain outside exported evidence.
  Copy-PrivateProfile $SeedProfile $privateProfile (Resolve-Path -LiteralPath $EvidenceDir).Path
  $receipt.seed_profile=@{path=(Resolve-Path -LiteralPath $SeedProfile).Path;license_present=(Test-Path -LiteralPath (Join-Path $portable 'reaper-license.rk'))}
  Copy-Item -LiteralPath $plugin -Destination $scan -Recurse
  $staged=Join-Path $scan (Split-Path $plugin -Leaf)
  foreach($sidecar in $SidecarPath){
    if(-not(Test-Path -LiteralPath $sidecar -PathType Leaf)){throw "Sidecar missing: $sidecar"}
    $destination=Join-Path $scan (Split-Path $sidecar -Leaf)
    if(Test-Path -LiteralPath $destination){throw 'Sidecar filename collision'}
    Copy-Item -LiteralPath $sidecar -Destination $destination
  }
  $manifest=@(Get-ChildItem -LiteralPath $scan -File -Recurse | Sort-Object FullName | ForEach-Object {
    @{path=$_.FullName;relative=$_.FullName.Substring($scan.Length+1);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
  })
  $receipt.staged_manifest=$manifest
  $ini=Join-Path $portable 'reaper.ini'
  $lines=@(Get-Content -LiteralPath $ini)
  $section=$null;$out=New-Object 'System.Collections.Generic.List[string]';$inserted=$false
  foreach($line in $lines){
    if($line -match '^\[(.+)\]$'){$section=$Matches[1]}
    if($section -eq 'REAPER' -and $line -match '^(vstpath64|vstpath_winarm64|clap_path(?:_windows-[^=]+)?|lastproject|loadlastproj|vstfullstate)='){continue}
    $out.Add($line)
    if($line -eq '[REAPER]'){
      # Staged root last wins duplicate VST3 class IDs; loaded-PE binding below
      # remains authoritative even when system roots are scanned implicitly.
      $out.Add($keys[0]+'='+(Get-ReaperVstScanPath $scan))
      $out.Add($keys[1]+'='+$scan);$out.Add('vstfullstate=33605');$out.Add('lastproject=');$out.Add('loadlastproj=0');$inserted=$true
    }
  }
  if(-not $inserted){throw 'Seed profile lacks REAPER section'}
  Write-ReaperIni $ini $out.ToArray()
  $status=Join-Path $run 'status.txt';$lua=Join-Path $PSScriptRoot 'windows_editor_smoke.lua'
  $env:PULP_DAW_SMOKE_FX="$(if($Format -eq 'vst3'){'VST3:'}else{'CLAP:'}) $PluginName"
  $env:PULP_DAW_SMOKE_STATUS=$status;$env:PULP_DAW_SMOKE_TOKEN=$token
  $env:PULP_DAW_SMOKE_PROJECT=Join-Path $run 'host-state.rpp'
  $env:PULP_DAW_SMOKE_PARAM_INDEX="$ParamIndex";$env:PULP_DAW_SMOKE_DURATION="$AutomationSec"
  $warm=Start-Owned @('-newinst','-cfgfile',$ini,'-new','-nosplash') 'warm'
  $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSec);$cacheMatch=$false
  while([DateTime]::UtcNow -lt $deadline){
    Assert-OwnedIdentity $warm.process $warm.started
    $cacheText=(Get-ChildItem -LiteralPath $portable -File | Where-Object {$_.Name -match '^reaper-(vstplugins|clap).*\.ini$'} | Get-Content) -join "`n"
    if($cacheText -match [regex]::Escape([IO.Path]::GetFileName($staged))){$cacheMatch=$true;break}
    Start-Sleep -Milliseconds 250
  }
  if(-not $cacheMatch){throw 'Scan cache did not acquire staged basename'}
  Stop-Owned $warm
  # A complete copied REAPER profile is required here. A minimal profile can
  # stall before startup-script dispatch; with the private resource directory
  # and its stale scan caches removed, the documented positional Lua form
  # executes in the same owned host process that will load the staged module.
  $proc=Start-Owned @('-newinst','-cfgfile',$ini,'-new','-nosplash',$lua) 'scripted'
  $receipt.process=@{pid=$proc.process.Id;started_utc=$proc.started;arguments=$proc.command;session_id=$proc.session_id}
  $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSec);$hostStatus=$null
  while([DateTime]::UtcNow -lt $deadline){
    Assert-OwnedIdentity $proc.process $proc.started
    $hostStatus=Read-HostStatus $status $token
    if($hostStatus -and ($hostStatus.fx_shown -eq '1' -or $hostStatus.error)){break}
    Start-Sleep -Milliseconds 100
  }
  if(-not $hostStatus -or $hostStatus.error){throw "Missing/failed insertion handshake: $($hostStatus.error)"}
  # Start the action deadline after observing insertion, independently of launch.
  $deadline=[DateTime]::UtcNow.AddSeconds($AutomationSec+$TimeoutSec)
  $terminal=if($ParamIndex -ge 0){'AUTOMATION_DONE'}else{'EDITOR_READY'}
  while([DateTime]::UtcNow -lt $deadline){
    Assert-OwnedIdentity $proc.process $proc.started
    $hostStatus=Read-HostStatus $status $token
    if($hostStatus -and ($hostStatus.phase -eq $terminal -or $hostStatus.error)){break}
    Start-Sleep -Milliseconds 100
  }
  Assert-TerminalStatus $hostStatus $ParamIndex
  $receipt.host_status=$hostStatus
  $modules=@($proc.process.Modules | Where-Object {[IO.Path]::GetExtension($_.FileName) -ieq ('.'+$Format)})
  $bound=@()
  foreach($module in $modules){
    $hash=(Get-FileHash -LiteralPath $module.FileName -Algorithm SHA256).Hash
    Assert-ModuleBinding $module.FileName $hash $manifest
    $pluginPe=Get-PeIdentity $module.FileName
    if($pluginPe.execution_abi -eq 'unknown'){throw 'Loaded plugin ABI metadata is unavailable'}
    $bound+=@{path=$module.FileName;sha256=$hash;pe=$pluginPe}
  }
  if($bound.Count -ne 1){throw 'Expected exactly one manifest-bound loaded plugin PE'}
  $receipt.loaded_plugin=$bound[0]
  Add-Type -AssemblyName System.Drawing
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OwnedWindow {
 [StructLayout(LayoutKind.Sequential)] public struct Point {public int X,Y;public Point(int x,int y){X=x;Y=y;}}
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int command);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point p);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsChild(IntPtr parent,IntPtr child);
 [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h,uint command);
 public static bool Belongs(IntPtr editor,IntPtr window){
  if(window==editor||IsChild(editor,window))return true;
  for(int i=0;i<32&&window!=IntPtr.Zero;i++){window=GetWindow(window,4);if(window==editor||IsChild(editor,window))return true;}
  return false;
 }
 public static bool IsOwned(IntPtr h,uint pid){uint p;GetWindowThreadProcessId(h,out p);return p==pid;}
 public static void GuardPoint(IntPtr editor,uint pid,int x,int y){var w=WindowFromPoint(new Point(x,y));if(!IsOwned(w,pid)||!Belongs(editor,w))throw new Exception("Point is outside bound editor");}
}
'@
  $editor=[IntPtr]([long]$hostStatus.editor_hwnd)
  function Guard-EditorOwner([long]$Hwnd) {
    Assert-OwnedIdentity $proc.process $proc.started
    if ($proc.process.SessionId -ne [int]$receipt.session.session_id) { throw 'Bound REAPER process left the interactive session' }
    if($Hwnd -ne $editor.ToInt64() -or $proc.process.Path -ine $reaper){throw 'Bound editor/host identity changed'}
    $current=@($proc.process.Modules | Where-Object {$_.FileName -ieq $receipt.loaded_plugin.path})
    if($current.Count -ne 1){throw 'Bound plugin module disappeared'}
    Assert-ModuleBinding $current[0].FileName (Get-FileHash -LiteralPath $current[0].FileName -Algorithm SHA256).Hash $manifest
    Assert-EditorWindow $Hwnd ([OwnedWindow]::IsWindow($editor)) ([OwnedWindow]::IsWindowVisible($editor)) ([OwnedWindow]::IsOwned($editor,[uint32]$proc.process.Id))
  }
  $receipt.editor_activation=[ordered]@{editor_hwnd=$editor.ToInt64()}
  Invoke-OwnedEditorActivation $editor.ToInt64() ${function:Guard-EditorOwner} {
    param([long]$Hwnd)
    $foreground=[OwnedWindow]::GetForegroundWindow()
    [uint32]$foregroundPid=0
    [OwnedWindow]::GetWindowThreadProcessId($foreground,[ref]$foregroundPid)|Out-Null
    if ($foregroundPid -ne 0) {
      try {
        $foregroundSession=[Diagnostics.Process]::GetProcessById([int]$foregroundPid).SessionId
        $receipt.editor_activation.foreground_session_id=$foregroundSession
      } catch { $receipt.editor_activation.foreground_session_id=$null }
    }
    $receipt.editor_activation.foreground_hwnd=$foreground.ToInt64()
    $receipt.editor_activation.foreground_pid=$foregroundPid
    [OwnedWindow]::Belongs([IntPtr]$Hwnd,$foreground)
  } {
    param([long]$Hwnd)
    [OwnedWindow]::ShowWindow([IntPtr]$Hwnd,9) | Out-Null
    [OwnedWindow]::SetForegroundWindow([IntPtr]$Hwnd)
  } -Observation $receipt.editor_activation -MaxObservations 51 | Out-Null
  function Guard-Editor {
    Guard-EditorOwner $editor.ToInt64()
    Assert-EditorEvidence $editor.ToInt64() ([OwnedWindow]::IsWindow($editor)) ([OwnedWindow]::IsWindowVisible($editor)) ([OwnedWindow]::IsOwned($editor,[uint32]$proc.process.Id)) ([OwnedWindow]::Belongs($editor,[OwnedWindow]::GetForegroundWindow()))
  }
  function Capture([string]$Name){
    Assert-OwnedIdentity $proc.process $proc.started
    Guard-Editor
    $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $image=New-Object Drawing.Bitmap($b.Width,$b.Height);$g=[Drawing.Graphics]::FromImage($image)
    try{$g.CopyFromScreen($b.Location,[Drawing.Point]::Empty,$b.Size);$image.Save((Join-Path $run $Name),[Drawing.Imaging.ImageFormat]::Png)}finally{$g.Dispose();$image.Dispose()}
  }
  Capture 'editor-before-context.png'
  $receipt.editor_hwnd=$editor.ToInt64();$receipt.foreground_hwnd=[OwnedWindow]::GetForegroundWindow().ToInt64()
  if($ContextX -ge 0 -and $ContextY -ge 0){
    Guard-Editor
    [OwnedWindow]::GuardPoint($editor,[uint32]$proc.process.Id,$ContextX,$ContextY)
    if(-not[OwnedWindow]::SetCursorPos($ContextX,$ContextY)){throw 'Pointer positioning failed'}
    [OwnedWindow]::mouse_event(8,0,0,0,[UIntPtr]::Zero);[OwnedWindow]::mouse_event(16,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 300;Capture 'editor-context-menu.png'
    Guard-Editor
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
  }
  $receipt.verdict='PASS';$receipt.product_ui_acceptance='NOT_EVALUATED';$receipt.live_audio='NOT_EVALUATED';$exitCode=0
} catch {$receipt.error=$_.Exception.Message;Write-Warning $receipt.error} finally {
  foreach($entry in $owned){try{Stop-Owned $entry}catch{$receipt.cleanup_errors+= $_.Exception.Message;$exitCode=3;$receipt.verdict='INCONCLUSIVE'}}
  if($privateProfile -and (Test-Path -LiteralPath $privateProfile)){
    try{Remove-Item -LiteralPath $privateProfile -Recurse -Force}catch{$receipt.cleanup_errors+='Private profile cleanup failed';$exitCode=3;$receipt.verdict='INCONCLUSIVE'}
  }
  foreach($name in $envNames){[Environment]::SetEnvironmentVariable($name,$priorEnv[$name],'Process')}
  $receipt.finished_utc=[DateTime]::UtcNow.ToString('o')
  $receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $run 'receipt.json') -Encoding utf8
}
Write-Output "$($receipt.verdict) evidence=$run"
exit $exitCode
