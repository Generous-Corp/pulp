# Pure receipt/identity helpers, also exercised by fixture-only tests.
function Get-PeMachine([string]$Path) {
  $stream = [IO.File]::OpenRead($Path)
  $reader = New-Object IO.BinaryReader($stream)
  try {
    if ($reader.ReadUInt16() -ne 0x5a4d) { throw 'Missing DOS signature' }
    $stream.Position = 0x3c; $offset = $reader.ReadUInt32()
    if ($offset -gt $stream.Length - 6) { throw 'Invalid PE offset' }
    $stream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550) { throw 'Missing PE signature' }
    switch ($reader.ReadUInt16()) {
      0x8664 { return 'x64' }
      0xaa64 { return 'arm64' }
      0xa641 { return 'arm64ec' }
      0xa64e { return 'arm64x' }
      default { throw 'Unsupported PE architecture' }
    }
  } finally { $reader.Dispose(); $stream.Dispose() }
}
function Get-ReaperScanKeys([string]$Machine) {
  switch ($Machine) {
    'x64' { return @('vstpath64', 'clap_path') }
    { $_ -in @('arm64','arm64ec','arm64x') } { return @('vstpath_winarm64', 'clap_path') }
    default { throw 'Unsupported REAPER architecture' }
  }
}
function Join-WindowsArguments([string[]]$Arguments) {
  return (($Arguments | ForEach-Object {
    if ($_ -match '["\r\n\x00]') { throw 'Invalid character in command argument' }
    '"' + ($_ -replace '(\\+)$','$1$1') + '"'
  }) -join ' ')
}
function Read-HostStatus([string]$Path, [string]$Token) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
  $s = @{}
  try {
    # The Lua writer replaces the status file atomically, so a reader can pass
    # Test-Path just before the old file is removed. Treat that narrow window
    # as "not published yet"; turning it into a harness failure kills REAPER
    # while the host is still delivering the valid automation receipt.
    foreach ($line in Get-Content -LiteralPath $Path) {
      $parts = $line -split '=', 2
      if ($parts.Count -eq 2) { $s[$parts[0]] = $parts[1] }
    }
  } catch [System.Management.Automation.ItemNotFoundException] {
    return $null
  } catch [System.UnauthorizedAccessException] {
    return $null
  } catch [System.IO.IOException] {
    return $null
  }
  if ($s.token -ne $Token) { throw 'Stale or foreign host status token' }
  return $s
}
function Assert-TerminalStatus($Status, [int]$ParamIndex) {
  if ($null -eq $Status -or $Status.fx_shown -ne '1') { throw 'Missing editor insertion handshake' }
  if ($Status.error) { throw ('Host error: ' + $Status.error) }
  if (-not $Status.state_saved -or -not (Test-Path -LiteralPath $Status.state_saved -PathType Leaf)) { throw 'Saved host state missing' }
  if ($ParamIndex -lt 0) {
    if ($Status.phase -ne 'EDITOR_READY') { throw 'Editor did not reach terminal status' }
    return
  }
  if ($Status.phase -ne 'AUTOMATION_DONE') { throw 'Host automation did not finish' }
  if ([int]$Status.param_index -ne $ParamIndex -or [int]$Status.param_count -le $ParamIndex) { throw 'Host parameter index/count mismatch' }
  if ([int]$Status.observed_changes -lt 2 -or [int]$Status.observations -lt 3) { throw 'Host parameter did not change on readback' }
  if (-not $Status.ContainsKey('max_error') -or [double]::IsNaN([double]$Status.max_error) -or [double]::IsInfinity([double]$Status.max_error) -or [double]$Status.max_error -gt 0.0001) { throw 'Host normalized readback disagrees with requested value' }
  if (-not $Status.state_saved -or -not (Test-Path -LiteralPath $Status.state_saved -PathType Leaf)) { throw 'Saved host state missing' }
}
function Assert-ModuleBinding([string]$LoadedPath, [string]$LoadedHash, $Manifest) {
  $entries = @($Manifest | Where-Object { $_.path -ieq $LoadedPath })
  if ($entries.Count -ne 1 -or $entries[0].sha256 -ine $LoadedHash) { throw 'Loaded module does not match exact staged manifest' }
  if ([IO.Path]::GetExtension($LoadedPath) -notin @('.vst3','.clap')) { throw 'Loaded module is not a plugin PE' }
}
function Invoke-BoundedExternal([string]$FilePath, [string[]]$ArgumentList = @(), [ValidateRange(1,60)][int]$TimeoutSec = 15, [string]$TimeoutError = 'EXTERNAL_PROBE_TIMEOUT') {
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = $FilePath
  $psi.Arguments = Join-WindowsArguments $ArgumentList
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $child = New-Object Diagnostics.Process
  $child.StartInfo = $psi
  $started = $false
  try {
    $started = $child.Start()
    if (-not $started) { throw 'EXTERNAL_PROBE_START_FAILED' }
    # Begin draining both redirected pipes before waiting. Toolhelp emits one
    # path per module; waiting for process exit first can fill a Windows pipe
    # buffer and deadlock the helper before it can return its module list.
    $stdoutTask = $child.StandardOutput.ReadToEndAsync()
    $stderrTask = $child.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while (-not $child.WaitForExit(100)) {
      if ([DateTime]::UtcNow -ge $deadline) {
        try { $child.Kill() } catch {}
        try { $child.WaitForExit(1000) } catch {}
        throw $TimeoutError
      }
    }
    # A process can exit while a descendant still owns an inherited pipe
    # handle. Keep the output-drain wait inside the same deadline instead of
    # allowing GetResult() to turn that condition into an unbounded wait.
    $remainingMs = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $stdoutTask.Wait($remainingMs)) {
      throw $TimeoutError
    }
    $remainingMs = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $stderrTask.Wait($remainingMs)) {
      throw $TimeoutError
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [pscustomobject]@{exit_code=$child.ExitCode;stdout=$stdout;stderr=$stderr}
  } finally {
    if ($started) {
      try {
        if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit(1000) | Out-Null }
      } catch {}
    }
    $child.Dispose()
  }
}
function Get-BoundModulePaths([int]$TargetPid, [ValidateRange(1,60)][int]$TimeoutSec = 15, [string]$TargetAbi = 'unknown') {
  # Process.Modules can block indefinitely while a GPU plugin is inside its
  # editor startup path. Keep the potentially blocked call in an independently
  # killable process; PowerShell jobs can hang during Stop-Job/Remove-Job and
  # leave the REAPER owner orphaned before its receipt is written.
  # Toolhelp32 (CreateToolhelp32Snapshot + Module32FirstW/NextW) avoids the
  # cross-bitness Process.Modules hang observed in ARM64EC REAPER. The child
  # retries ERROR_BAD_LENGTH (ErrorBadLength) at most three times and rejects
  # more than MaxModules entries.
  # Keep the probe in the independently killable child so snapshot calls are bounded too.
  $toolhelpSource = 'dXNpbmcgU3lzdGVtOwp1c2luZyBTeXN0ZW0uQ29sbGVjdGlvbnMuR2VuZXJpYzsKdXNpbmcgU3lzdGVtLkNvbXBvbmVudE1vZGVsOwp1c2luZyBTeXN0ZW0uUnVudGltZS5JbnRlcm9wU2VydmljZXM7CnB1YmxpYyBzdGF0aWMgY2xhc3MgUHVscFRvb2xoZWxwTW9kdWxlUHJvYmUgewogICAgcHJpdmF0ZSBjb25zdCB1aW50IFNuYXBNb2R1bGUgPSAweDAwMDAwMDA4OwogICAgcHJpdmF0ZSBjb25zdCB1aW50IFNuYXBNb2R1bGUzMiA9IDB4MDAwMDAwMTA7CiAgICBwcml2YXRlIGNvbnN0IGludCBFcnJvck5vTW9yZUZpbGVzID0gMTg7CiAgICBwcml2YXRlIGNvbnN0IGludCBFcnJvckJhZExlbmd0aCA9IDI0OwogICAgcHJpdmF0ZSBjb25zdCBpbnQgTWF4TW9kdWxlcyA9IDQwOTY7CiAgICBbU3RydWN0TGF5b3V0KExheW91dEtpbmQuU2VxdWVudGlhbCwgQ2hhclNldCA9IENoYXJTZXQuVW5pY29kZSldCiAgICBwcml2YXRlIHN0cnVjdCBNb2R1bGVFbnRyeSB7CiAgICAgICAgcHVibGljIHVpbnQgU2l6ZTsKICAgICAgICBwdWJsaWMgdWludCBNb2R1bGVJZDsKICAgICAgICBwdWJsaWMgdWludCBQcm9jZXNzSWQ7CiAgICAgICAgcHVibGljIHVpbnQgR2xvYmFsVXNlQ291bnQ7CiAgICAgICAgcHVibGljIHVpbnQgUHJvY2Vzc1VzZUNvdW50OwogICAgICAgIHB1YmxpYyBJbnRQdHIgQmFzZUFkZHJlc3M7CiAgICAgICAgcHVibGljIHVpbnQgQmFzZVNpemU7CiAgICAgICAgcHVibGljIEludFB0ciBNb2R1bGVIYW5kbGU7CiAgICAgICAgW01hcnNoYWxBcyhVbm1hbmFnZWRUeXBlLkJ5VmFsVFN0ciwgU2l6ZUNvbnN0ID0gMjU2KV0gcHVibGljIHN0cmluZyBNb2R1bGVOYW1lOwogICAgICAgIFtNYXJzaGFsQXMoVW5tYW5hZ2VkVHlwZS5CeVZhbFRTdHIsIFNpemVDb25zdCA9IDI2MCldIHB1YmxpYyBzdHJpbmcgRXhlUGF0aDsKICAgIH0KICAgIFtEbGxJbXBvcnQoImtlcm5lbDMyLmRsbCIsIFNldExhc3RFcnJvciA9IHRydWUpXQogICAgcHJpdmF0ZSBzdGF0aWMgZXh0ZXJuIEludFB0ciBDcmVhdGVUb29saGVscDMyU25hcHNob3QodWludCBmbGFncywgdWludCBwcm9jZXNzSWQpOwogICAgW0RsbEltcG9ydCgia2VybmVsMzIuZGxsIiwgU2V0TGFzdEVycm9yID0gdHJ1ZSwgQ2hhclNldCA9IENoYXJTZXQuVW5pY29kZSldCiAgICBwcml2YXRlIHN0YXRpYyBleHRlcm4gYm9vbCBNb2R1bGUzMkZpcnN0VyhJbnRQdHIgc25hcHNob3QsIHJlZiBNb2R1bGVFbnRyeSBlbnRyeSk7CiAgICBbRGxsSW1wb3J0KCJrZXJuZWwzMi5kbGwiLCBTZXRMYXN0RXJyb3IgPSB0cnVlLCBDaGFyU2V0ID0gQ2hhclNldC5Vbmljb2RlKV0KICAgIHByaXZhdGUgc3RhdGljIGV4dGVybiBib29sIE1vZHVsZTMyTmV4dFcoSW50UHRyIHNuYXBzaG90LCByZWYgTW9kdWxlRW50cnkgZW50cnkpOwogICAgW0RsbEltcG9ydCgia2VybmVsMzIuZGxsIiwgU2V0TGFzdEVycm9yID0gdHJ1ZSldCiAgICBwcml2YXRlIHN0YXRpYyBleHRlcm4gYm9vbCBDbG9zZUhhbmRsZShJbnRQdHIgaGFuZGxlKTsKICAgIHB1YmxpYyBzdGF0aWMgc3RyaW5nW10gUGF0aHMoaW50IHByb2Nlc3NJZCwgdWludCBzbmFwc2hvdEZsYWdzKSB7CiAgICAgICAgSW50UHRyIHNuYXBzaG90ID0gSW50UHRyLlplcm87CiAgICAgICAgaW50IHNuYXBzaG90RXJyb3IgPSAwOwogICAgICAgIGZvciAoaW50IGF0dGVtcHQgPSAwOyBhdHRlbXB0IDwgMzsgYXR0ZW1wdCsrKSB7CiAgICAgICAgICAgIHNuYXBzaG90ID0gQ3JlYXRlVG9vbGhlbHAzMlNuYXBzaG90KHNuYXBzaG90RmxhZ3MsIHVuY2hlY2tlZCgodWludClwcm9jZXNzSWQpKTsKICAgICAgICAgICAgaWYgKHNuYXBzaG90ICE9IEludFB0ci5aZXJvICYmIHNuYXBzaG90LlRvSW50NjQoKSAhPSAtMSkgYnJlYWs7CiAgICAgICAgICAgIHNuYXBzaG90RXJyb3IgPSBNYXJzaGFsLkdldExhc3RXaW4zMkVycm9yKCk7CiAgICAgICAgICAgIGlmIChzbmFwc2hvdEVycm9yICE9IEVycm9yQmFkTGVuZ3RoIHx8IGF0dGVtcHQgPT0gMikgdGhyb3cgbmV3IFdpbjMyRXhjZXB0aW9uKHNuYXBzaG90RXJyb3IsICJNT0RVTEVfU05BUFNIT1RfRkFJTEVEIik7CiAgICAgICAgICAgIFN5c3RlbS5UaHJlYWRpbmcuVGhyZWFkLlNsZWVwKDI1KTsKICAgICAgICB9CiAgICAgICAgdHJ5IHsKICAgICAgICAgICAgdmFyIGVudHJ5ID0gbmV3IE1vZHVsZUVudHJ5IHsgU2l6ZSA9ICh1aW50KU1hcnNoYWwuU2l6ZU9mKHR5cGVvZihNb2R1bGVFbnRyeSkpIH07CiAgICAgICAgICAgIGlmICghTW9kdWxlMzJGaXJzdFcoc25hcHNob3QsIHJlZiBlbnRyeSkpIHRocm93IG5ldyBXaW4zMkV4Y2VwdGlvbihNYXJzaGFsLkdldExhc3RXaW4zMkVycm9yKCksICJNb2R1bGUzMkZpcnN0VyIpOwogICAgICAgICAgICB2YXIgcGF0aHMgPSBuZXcgTGlzdDxzdHJpbmc+KCk7CiAgICAgICAgICAgIGRvIHsKICAgICAgICAgICAgICAgIGlmICghU3RyaW5nLklzTnVsbE9yRW1wdHkoZW50cnkuRXhlUGF0aCkpIHsKICAgICAgICAgICAgICAgICAgICBpZiAocGF0aHMuQ291bnQgPj0gTWF4TW9kdWxlcykgdGhyb3cgbmV3IEludmFsaWRPcGVyYXRpb25FeGNlcHRpb24oIk1PRFVMRV9DT1VOVF9MSU1JVCIpOwogICAgICAgICAgICAgICAgICAgIHBhdGhzLkFkZChlbnRyeS5FeGVQYXRoKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIGVudHJ5ID0gbmV3IE1vZHVsZUVudHJ5IHsgU2l6ZSA9ICh1aW50KU1hcnNoYWwuU2l6ZU9mKHR5cGVvZihNb2R1bGVFbnRyeSkpIH07CiAgICAgICAgICAgIH0gd2hpbGUgKE1vZHVsZTMyTmV4dFcoc25hcHNob3QsIHJlZiBlbnRyeSkpOwogICAgICAgICAgICBpbnQgZXJyb3IgPSBNYXJzaGFsLkdldExhc3RXaW4zMkVycm9yKCk7CiAgICAgICAgICAgIGlmIChlcnJvciAhPSBFcnJvck5vTW9yZUZpbGVzKSB0aHJvdyBuZXcgV2luMzJFeGNlcHRpb24oZXJyb3IsICJNb2R1bGUzMk5leHRXIik7CiAgICAgICAgICAgIHJldHVybiBwYXRocy5Ub0FycmF5KCk7CiAgICAgICAgfSBmaW5hbGx5IHsgQ2xvc2VIYW5kbGUoc25hcHNob3QpOyB9CiAgICB9Cn0='
  # ARM64EC is x64 COFF/CHPE, not WOW64. Asking for SNAPMODULE32 while
  # snapshotting that target can hang on the native ARM host. Use the normal
  # module view for known x64/ARM64EC/ARM64X targets; retain both views only
  # for unknown or explicitly 32-bit callers.
  $snapshotFlags = if ($TargetAbi -in @('x64','arm64ec','arm64x')) { 0x8 } else { 0x18 }
  $probe = '$code=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("' + $toolhelpSource + '"));Add-Type -TypeDefinition $code -ErrorAction Stop;[PulpToolhelpModuleProbe]::Paths(' + $TargetPid + ',' + $snapshotFlags + ')'
  $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($probe))
  $powershell = Join-Path $PSHOME 'powershell.exe'
  if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    $command = @(Get-Command pwsh.exe,pwsh -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($command.Count -ne 1) { throw 'POWERSHELL_PROBE_UNAVAILABLE' }
    $powershell = if ($command[0].Source) { $command[0].Source } else { $command[0].Path }
  }
  $result = Invoke-BoundedExternal $powershell @('-NoProfile','-NonInteractive','-EncodedCommand',$encoded) $TimeoutSec 'MODULE_ENUMERATION_TIMEOUT'
  if ($result.exit_code -ne 0) {
    $detail = $result.stderr.Trim()
    if ($detail.Length -gt 512) { $detail = $detail.Substring(0,512) }
    throw ('MODULE_ENUMERATION_FAILED: ' + $detail)
  }
  return @($result.stdout -split "`r?`n" | Where-Object { $_ })
}
function Assert-OwnedIdentity($Process, [string]$StartedUtc) {
  $Process.Refresh()
  if ($Process.HasExited -or $Process.StartTime.ToUniversalTime().ToString('o') -ne $StartedUtc) { throw 'Owned process identity changed' }
}
function Get-InteractiveDesktopContext {
  $sessionId = [Diagnostics.Process]::GetCurrentProcess().SessionId
  $explorer = @(Get-Process -Name explorer -ErrorAction SilentlyContinue |
    Where-Object { $_.SessionId -eq $sessionId })
  if ($explorer.Count -eq 0) { throw 'INTERACTIVE_SESSION_ABSENT' }
  [ordered]@{
    session_id = $sessionId
    explorer_pids = @($explorer | ForEach-Object { [int]$_.Id })
  }
}

function Copy-PrivateProfile([string]$Source, [string]$Destination, [string]$EvidenceRoot) {
  $private = [IO.Path]::GetFullPath($Destination)
  $export = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
  if ($private.StartsWith($export, [StringComparison]::OrdinalIgnoreCase) -or $private -eq $export.TrimEnd([IO.Path]::DirectorySeparatorChar)) { throw 'Private profile cannot be inside exported evidence' }
  if ([IO.Path]::GetFullPath($Source) -eq $private) { throw 'Private profile cannot replace source profile' }
  New-Item -ItemType Directory -Path $private | Out-Null
  # `-cfgfile` selects the alternate REAPER resource directory, not just an
  # isolated INI. Copy the non-secret profile so startup scripts and scan
  # caches resolve beside the selected reaper.ini rather than falling back to
  # the user's profile. Cache files are deliberately removed: a stale entry
  # can make REAPER load an old Spectr backup before the staged exact module.
  Get-ChildItem -LiteralPath $Source -Force | Where-Object {
    $_.Name -ne 'reaper-license.rk'
  } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $private $_.Name) -Recurse -Force
  }
  $license = Join-Path $Source 'reaper-license.rk'
  if (Test-Path -LiteralPath $license -PathType Leaf) {
    Copy-Item -LiteralPath $license -Destination (Join-Path $private 'reaper-license.rk')
  }
  Get-ChildItem -LiteralPath $private -File | Where-Object {
    $_.Name -match '^reaper-(vstplugins|clap).*\.ini$'
  } | Remove-Item -Force
}
function Assert-EditorWindow([long]$Hwnd, [bool]$Exists, [bool]$Visible, [bool]$Owned) {
  if ($Hwnd -eq 0 -or -not $Exists) { throw 'Floating editor HWND absent' }
  if (-not $Visible) { throw 'Floating editor is hidden' }
  if (-not $Owned) { throw 'Floating editor belongs to another process' }
}
function Assert-EditorEvidence([long]$Hwnd, [bool]$Exists, [bool]$Visible, [bool]$Owned, [bool]$ForegroundBelongs) {
  Assert-EditorWindow $Hwnd $Exists $Visible $Owned
  if (-not $ForegroundBelongs) { throw 'Foreground is not the bound editor or its child/popup' }
}
function Invoke-OwnedEditorActivation([long]$Hwnd, [scriptblock]$ValidateOwner, [scriptblock]$ForegroundBelongs, [scriptblock]$Activate,
  [System.Collections.IDictionary]$Observation = [ordered]@{}, [ValidateRange(1,51)][int]$MaxObservations = 1,
  [scriptblock]$Pause = {Start-Sleep -Milliseconds 100}) {
  # The real validator binds process start, loaded PE and HWND ownership before
  # any focus request. Injected operations let fixtures prove refusal ordering.
  & $ValidateOwner $Hwnd
  $Observation.requested=-not [bool](& $ForegroundBelongs $Hwnd)
  $Observation.api_result=$null
  $Observation.foreground_belongs=$false
  $Observation.observations=0
  if($Observation.requested){$Observation.api_result=[bool](& $Activate $Hwnd)}
  for($i=0;$i -lt $MaxObservations;$i++){
    & $ValidateOwner $Hwnd
    $Observation.observations++
    $Observation.foreground_belongs=[bool](& $ForegroundBelongs $Hwnd)
    if($Observation.foreground_belongs){return $Observation}
    if($i+1 -lt $MaxObservations){& $Pause}
  }
  throw 'Foreground is not the bound editor or its child/popup after activation'
}

# ARM64EC images commonly retain AMD64 COFF machine 0x8664. Keep that raw
# identity separate from the ABI derived from validated CHPE load metadata.
function Get-PeIdentity([string]$Path) {
  $rawMachine=Get-PeMachine $Path
  $result=[ordered]@{coff_machine=$rawMachine;coff_machine_hex=$null;optional_magic=$null;execution_abi=$rawMachine;metadata_status='absent';chpe_metadata_pointer=$null;chpe_metadata_version=$null;code_map_rva=$null;code_map_count=$null;code_composition='not_inferred'}
  $f=[IO.File]::OpenRead($Path);$b=New-Object IO.BinaryReader($f)
  try {
    $f.Position=60;$nt=$b.ReadUInt32();$f.Position=$nt+4;$result.coff_machine_hex=('0x{0:X4}' -f $b.ReadUInt16());$sections=$b.ReadUInt16()
    $f.Position=$nt+20;$optionalSize=$b.ReadUInt16();$optional=$nt+24;$sectionStart=$optional+$optionalSize
    if($optionalSize -lt 112 -or $sections -gt 96 -or $sectionStart+$sections*40 -gt $f.Length){throw 'Incomplete PE optional header/section table'}
    $f.Position=$optional;$magic=$b.ReadUInt16();$result.optional_magic=('0x{0:X}' -f $magic)
    if($magic -ne 0x20b){throw 'Expected PE32+ optional header'}
    $f.Position=$optional+24;$imageBase=$b.ReadUInt64();$result.image_base=('0x{0:X}' -f $imageBase)
    $f.Position=$optional+108;$directoryCount=$b.ReadUInt32()
    function Find-PeOffset([uint32]$Rva,[long]$Length) {
      for($i=0;$i -lt $sections;$i++){
        $f.Position=$sectionStart+$i*40+8;$virtualSize=$b.ReadUInt32();$virtualAddress=$b.ReadUInt32();$rawSize=$b.ReadUInt32();$rawPointer=$b.ReadUInt32()
        if($Rva -ge $virtualAddress -and $Rva-$virtualAddress -lt [Math]::Max($virtualSize,$rawSize)){
          $delta=[long]$Rva-$virtualAddress;$offset=[long]$rawPointer+$delta
          if($delta+$Length -gt $rawSize -or $offset+$Length -gt $f.Length){throw 'Metadata RVA exceeds file-backed section'}
          return $offset
        }
      }
      throw 'Metadata RVA is not mapped'
    }
    if($directoryCount -le 10){return [pscustomobject]$result}
    if($optionalSize -lt 200){throw 'Incomplete load-config directory entry'}
    $f.Position=$optional+192;$loadRva=$b.ReadUInt32();$loadSize=$b.ReadUInt32()
    $result.load_config_rva=$loadRva;$result.load_config_size=$loadSize
    if($loadRva -eq 0 -and $loadSize -eq 0){return [pscustomobject]$result}
    if($loadRva -eq 0 -or $loadSize -lt 4){throw 'Invalid load-config directory'}
    $loadOffset=Find-PeOffset $loadRva $loadSize;$f.Position=$loadOffset;$configSize=$b.ReadUInt32()
    if($configSize -gt $loadSize -or $configSize -lt 4){throw 'Invalid load-config structure size'}
    if($configSize -lt 208){return [pscustomobject]$result}
    $f.Position=$loadOffset+200;$chpe=$b.ReadUInt64();$result.chpe_metadata_pointer=('0x{0:X}' -f $chpe)
    if($chpe -eq 0){return [pscustomobject]$result}
    if($chpe -lt $imageBase -or $chpe-$imageBase -gt [uint32]::MaxValue){throw 'Invalid CHPE metadata VA'}
    $offset=Find-PeOffset ([uint32]($chpe-$imageBase)) 12;$f.Position=$offset
    $version=$b.ReadUInt32();$codeMap=$b.ReadUInt32();$count=$b.ReadUInt32()
    $result.chpe_metadata_version=$version;$result.metadata_file_offset=$offset;$result.code_map_rva=$codeMap;$result.code_map_count=$count
    if($version -notin @(1,2) -or $count -eq 0 -or $count -gt 100000){throw 'Unsupported/empty CHPE metadata'}
    $result.code_map_file_offset=Find-PeOffset $codeMap ([long]$count*8);$result.code_map_bytes=[long]$count*8
    $result.metadata_status='validated_chpe'
    if($rawMachine -eq 'x64'){$result.execution_abi='arm64ec'}
    return [pscustomobject]$result
  } catch {
    $result.execution_abi='unknown';$result.metadata_status='unavailable';$result.metadata_error=$_.Exception.Message
    return [pscustomobject]$result
  } finally {$b.Dispose();$f.Dispose()}
}

function Test-WindowsPluginAbiCompatible([string]$HostAbi, [string]$PluginAbi) {
  switch ($HostAbi) {
    'x64' { return $PluginAbi -eq 'x64' }
    'arm64' { return $PluginAbi -eq 'arm64' }
    { $_ -in @('arm64ec','arm64x') } { return $PluginAbi -in @('arm64ec','arm64x') }
    default { return $false }
  }
}

function Select-PluginPeCandidate([string]$PluginPath, [string]$Format, [string]$HostAbi) {
  $plugin = (Resolve-Path -LiteralPath $PluginPath -ErrorAction Stop).Path
  if (Test-Path -LiteralPath $plugin -PathType Leaf) {
    $candidates = @([pscustomobject]@{FullName=$plugin;Name=[IO.Path]::GetFileName($plugin)})
  } else {
    $bundleName = [IO.Path]::GetFileNameWithoutExtension($plugin)
    $expectedName = $bundleName + '.' + $Format
    $all = @(Get-ChildItem -LiteralPath $plugin -File -Recurse |
      Where-Object {
        $_.Extension -ieq ('.' + $Format) -or
        ($Format -eq 'vst3' -and $_.Extension -ieq '.dll')
      } | Sort-Object FullName)
    # A VST3 bundle normally contains <bundle-name>.vst3. Prefer that exact
    # payload name, but keep a generic extension fallback for valid bundles
    # whose producer chose a different inner filename. Dependencies are only
    # considered when no format-named payload exists.
    $named = @($all | Where-Object { $_.Name -ieq $expectedName })
    $candidates = if ($named.Count -gt 0) { $named } else { $all }
  }
  $compatible = @()
  foreach ($candidate in $candidates) {
    try {
      $pe = Get-PeIdentity $candidate.FullName
      if (Test-WindowsPluginAbiCompatible $HostAbi $pe.execution_abi) {
        $compatible += [pscustomobject]@{path=$candidate.FullName;pe=$pe}
      }
    } catch {
      # A sidecar/dependency that is not a PE is not a plugin payload candidate.
    }
  }
  if ($compatible.Count -eq 1) { return $compatible[0] }
  if ($compatible.Count -eq 0) {
    if ($candidates.Count -eq 0) { throw "PLUGIN_PE_PAYLOAD_MISSING: bundle=$plugin" }
    throw "PLUGIN_ABI_MISMATCH: host=$HostAbi bundle=$plugin"
  }
  throw "Ambiguous plugin PE payload: host=$HostAbi bundle=$plugin candidates=$($compatible.Count)"
}

# REAPER recognizes these literal default roots. Expanded absolute equivalents
# make it append the defaults again, after the staged root, changing precedence.
function Get-ReaperVstScanPath([string]$StagedRoot) {
  return '%COMMONPROGRAMFILES%\VST3;%LOCALAPPDATA%\Programs\Common\VST3;'+$StagedRoot
}
function Write-ReaperIni([string]$Path,[string[]]$Lines) {
  # Windows PowerShell 5.1 Set-Content -Encoding utf8 emits a BOM. Preserve the
  # no-BOM INI form read by the seeded REAPER profile on the actual guest.
  [IO.File]::WriteAllLines($Path,$Lines,(New-Object Text.UTF8Encoding($false)))
}
function Assert-ReaperSeedProfile([string]$SeedProfile) {
  $ini=Join-Path $SeedProfile 'reaper.ini'
  if(-not(Test-Path -LiteralPath $ini -PathType Leaf)){throw 'REAPER_SEED_PROFILE_INCOMPLETE: reaper.ini is missing'}
  $text=[IO.File]::ReadAllText($ini)
  if(-not($text -match '(?m)^\[REAPER\]\s*$' -and $text -match '(?m)^\[audioconfig\]\s*$')){
    throw 'REAPER_SEED_PROFILE_INCOMPLETE: initialized reaper.ini must contain [REAPER] and [audioconfig] sections'
  }
}
function Write-Utf8NoBom([string]$Path,[string]$Text) {
  [IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false))
}
function Test-HalfOpenWindowRectContainsPoint([int[]]$Rect,[int]$X,[int]$Y) {
  if($null -eq $Rect -or $Rect.Count -ne 4){return $false}
  return $X -ge $Rect[0] -and $X -lt $Rect[2] -and $Y -ge $Rect[1] -and $Y -lt $Rect[3]
}
