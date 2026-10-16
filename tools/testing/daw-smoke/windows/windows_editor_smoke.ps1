<#!
.SYNOPSIS
  Bounded Windows REAPER evidence run for a format-qualified Pulp plugin.

This is an opt-in operator test. It never installs a plugin or kills a REAPER
process it did not start. A missing scan-cache entry, missing host status, or
missing interactive desktop is INCONCLUSIVE (exit 3), never PASS (exit 0).
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$ReaperExe,
  [Parameter(Mandatory=$true)][ValidateSet('vst3','clap')][string]$Format,
  [Parameter(Mandatory=$true)][string]$PluginPath,
  [Parameter(Mandatory=$true)][string]$PluginName,
  [Parameter(Mandatory=$true)][string]$EvidenceDir,
  [int]$ParamIndex = -1,
  [int]$TimeoutSec = 60,
  [int]$ContextX = -1,
  [int]$ContextY = -1
)
$ErrorActionPreference = 'Stop'
$ExitPass = 0; $ExitFail = 1; $ExitSkip = 2; $ExitInconclusive = 3

function Fail-Inconclusive([string]$Reason) {
  $script:Verdict = @{ verdict='INCONCLUSIVE'; reason=$Reason }
  Write-Warning $Reason
  exit $ExitInconclusive
}
function Require-File([string]$Path,[string]$Label) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail-Inconclusive "$Label not found: $Path" }
  return (Resolve-Path -LiteralPath $Path).Path
}

$reaper = Require-File $ReaperExe 'REAPER executable'
$plugin = (Resolve-Path -LiteralPath $PluginPath -ErrorAction SilentlyContinue)
if (-not $plugin) { Fail-Inconclusive "plugin not found: $PluginPath" }
$plugin = $plugin.Path
if ($Format -eq 'vst3' -and -not ($plugin -match '(?i)\.vst3$')) { Fail-Inconclusive 'vst3 run received a non-vst3 path' }
if ($Format -eq 'clap' -and -not ($plugin -match '(?i)\.clap$')) { Fail-Inconclusive 'clap run received a non-clap path' }
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$EvidenceDir = (Resolve-Path -LiteralPath $EvidenceDir).Path
$run = Join-Path $EvidenceDir ('run-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
$portable = Join-Path $run 'portable'; $scan = Join-Path $run 'scan'
New-Item -ItemType Directory -Force -Path $portable,$scan | Out-Null

# Stage the exact artifact into an isolated scan directory. REAPER's portable
# path keys are intentionally explicit; the cache is checked below before any
# host verdict is accepted.
if ($Format -eq 'vst3') { Copy-Item -LiteralPath $plugin -Destination $scan -Recurse }
else { Copy-Item -LiteralPath $plugin -Destination $scan }
$staged = Join-Path $scan (Split-Path $plugin -Leaf)
$sha = (Get-ChildItem -LiteralPath $staged -File -Recurse | Sort-Object FullName |
  Get-FileHash -Algorithm SHA256 | ForEach-Object Hash | Out-String).Trim()
$status = Join-Path $run 'status.txt'; $reaperOut = Join-Path $run 'reaper.out.txt'
$lua = Join-Path $PSScriptRoot 'windows_editor_smoke.lua'
$ini = Join-Path $portable 'reaper.ini'
@("[REAPER]","vstfullstate=33605","vstpath64=$scan","clap_path_windows-x86_64=$scan","lastproject=","loadlastproj=0") | Set-Content -LiteralPath $ini -Encoding ascii

$env:PULP_DAW_SMOKE_FX = "$(if ($Format -eq 'vst3') { 'VST3:' } else { 'CLAP:' })$PluginName"
$env:PULP_DAW_SMOKE_STATUS = $status
$env:PULP_DAW_SMOKE_PARAM_INDEX = "$ParamIndex"
$env:PULP_DAW_SMOKE_DURATION = "$TimeoutSec"
$args = @('-newinst','-cfgfile', $ini, $lua)
# REAPER executes a startup script only when it is the final positional input;
# `-script`/`-portable` are not Windows REAPER options. Warm the same profile
# first so plugin scanning is durable before the scripted instance starts.
$warm = Start-Process -FilePath $reaper -ArgumentList @('-newinst','-cfgfile',$portable) -WorkingDirectory $run -RedirectStandardOutput $reaperOut -RedirectStandardError ($reaperOut + '.err') -PassThru
try {
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $cacheFiles = Get-ChildItem -LiteralPath $portable -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '(?i)reaper-(vstplugins|clap).*\.ini$' }
    if ($cacheFiles.Count -gt 0) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($warm -and -not $warm.HasExited) { Stop-Process -Id $warm.Id -Force }
  $args = @('-newinst','-cfgfile', $portable, $lua)
  $proc = Start-Process -FilePath $reaper -ArgumentList $args -WorkingDirectory $run -RedirectStandardOutput $reaperOut -RedirectStandardError ($reaperOut + '.err') -PassThru
  while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $status)) { Start-Sleep -Milliseconds 250 }
  if (-not (Test-Path -LiteralPath $status)) { Fail-Inconclusive 'REAPER never reported editor status' }
  $hostStatus = Get-Content -Raw -LiteralPath $status
  # FX_SHOWN is only the insertion handshake. If modulation was requested,
  # wait for the Lua driver to finish and report the terminal marker; otherwise
  # a fast wrapper could PASS while the automation never ran.
  $final = if ($ParamIndex -ge 0) { 'MODULATION_DONE' } else { 'EDITOR_SHOWN_NO_MODULATION' }
  while ((Get-Date) -lt $deadline -and $hostStatus -notmatch [regex]::Escape($final)) {
    Start-Sleep -Milliseconds 250
    $hostStatus = Get-Content -Raw -LiteralPath $status
  }
  if ($hostStatus -notmatch [regex]::Escape($final)) { Fail-Inconclusive "REAPER did not reach terminal status $final (last: $hostStatus)" }
  if ($ParamIndex -ge 0) {
    if ($hostStatus -notmatch ('param_count=([0-9]+)') -or [int]$Matches[1] -le $ParamIndex) { Fail-Inconclusive "REAPER did not report the requested parameter index/count: $hostStatus" }
    if ($hostStatus -notmatch 'value=([0-9]+(?:\.[0-9]+)?)') { Fail-Inconclusive "REAPER did not report a final normalized parameter value: $hostStatus" }
  }
  $cache = Get-ChildItem -LiteralPath $portable -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '(?i)reaper-(vstplugins|clap).*\.ini$' } |
    Get-Content -ErrorAction SilentlyContinue
  $qualified = "$(if ($Format -eq 'vst3') { 'VST3:' } else { 'CLAP:' })$PluginName"
  $cacheText = ($cache -join "`n")
  $cachePath = ($scan -replace '\\','/')
  $cacheHasName = $cacheText -match [regex]::Escape($PluginName)
  $cacheHasPath = ($cacheText -replace '\\','/') -match [regex]::Escape($cachePath)
  if (-not $cacheHasName -or -not $cacheHasPath) { Fail-Inconclusive 'portable REAPER scan cache lacks the exact staged plugin path/name; host insertion is unproven' }
  if ($hostStatus -match 'FX_NOT_FOUND|TRACK_NOT_AVAILABLE') { Fail-Inconclusive $hostStatus }
  if ($hostStatus -notmatch 'FX_SHOWN') { Fail-Inconclusive "unexpected host status: $hostStatus" }

  # Screenshot requires an interactive Windows desktop. Capture the foreground
  # editor only; a headless/RDP-without-desktop run is explicitly incomplete.
  Add-Type -AssemblyName System.Drawing
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WindowProbe {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static string Title() { var b=new StringBuilder(512); GetWindowText(GetForegroundWindow(),b,b.Capacity); return b.ToString(); }
  public static RECT Rect() { RECT r; GetWindowRect(GetForegroundWindow(),out r); return r; }
}
"@
  $windowTitle = [WindowProbe]::Title()
  if ($windowTitle -notmatch [regex]::Escape($PluginName)) { Fail-Inconclusive "foreground window is not the Spectr editor (title: $windowTitle)" }
  $windowRect = [WindowProbe]::Rect()
  $screen = [System.Windows.Forms.Screen]::PrimaryScreen
  if ($null -eq $screen) { Fail-Inconclusive 'no interactive Windows desktop for screenshot' }
  $bmp = New-Object Drawing.Bitmap($screen.Bounds.Width,$screen.Bounds.Height)
  $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($screen.Bounds.Location,[Drawing.Point]::Empty,$screen.Bounds.Size); $g.Dispose()
  $before = Join-Path $run 'editor-before-context.png'; $bmp.Save($before,[Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  if ($ContextX -ge 0 -and $ContextY -ge 0) {
    if ($ContextX -lt $windowRect.Left -or $ContextX -gt $windowRect.Right -or $ContextY -lt $windowRect.Top -or $ContextY -gt $windowRect.Bottom) { Fail-Inconclusive 'context coordinates are outside the foreground plugin editor window' }
    [System.Windows.Forms.Cursor]::Position = New-Object Drawing.Point($ContextX,$ContextY)
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Mouse {
  [DllImport("user32.dll")]
  public static extern void mouse_event(uint f, uint dx, uint dy, uint d, uint e);
}
"@
    [Mouse]::mouse_event(0x0008,0,0,0,0); [Mouse]::mouse_event(0x0010,0,0,0,0); Start-Sleep -Milliseconds 300
    $bmp = New-Object Drawing.Bitmap($screen.Bounds.Width,$screen.Bounds.Height); $g=[Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($screen.Bounds.Location,[Drawing.Point]::Empty,$screen.Bounds.Size); $g.Dispose(); $bmp.Save((Join-Path $run 'editor-context-menu.png'),[Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
  }
  if ($ContextX -ge 0 -and $ContextY -ge 0 -and -not (Test-Path (Join-Path $run 'editor-context-menu.png'))) { Fail-Inconclusive 'context-menu screenshot was requested but not captured' }
  @{format=$Format; plugin=$plugin; staged=$staged; plugin_sha256=$sha; host_status=$hostStatus; scan_cache_match=$true; scan_cache_path_match=$cacheHasPath; window_title=$windowTitle; screenshot_before=(Test-Path (Join-Path $run 'editor-before-context.png')); context_screenshot=(Test-Path (Join-Path $run 'editor-context-menu.png')); verdict='PASS'} | ConvertTo-Json | Set-Content (Join-Path $run 'receipt.json')
  Write-Output "PASS evidence=$run"
  exit $ExitPass
} finally {
  if ($warm -and -not $warm.HasExited) { Stop-Process -Id $warm.Id -Force }
  if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
  Remove-Item Env:PULP_DAW_SMOKE_FX,Env:PULP_DAW_SMOKE_STATUS,Env:PULP_DAW_SMOKE_PARAM_INDEX,Env:PULP_DAW_SMOKE_DURATION -ErrorAction SilentlyContinue
}
