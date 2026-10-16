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
  foreach ($line in Get-Content -LiteralPath $Path) {
    $parts = $line -split '=', 2
    if ($parts.Count -eq 2) { $s[$parts[0]] = $parts[1] }
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
function Assert-OwnedIdentity($Process, [string]$StartedUtc) {
  $Process.Refresh()
  if ($Process.HasExited -or $Process.StartTime.ToUniversalTime().ToString('o') -ne $StartedUtc) { throw 'Owned process identity changed' }
}

function Copy-PrivateProfile([string]$Source, [string]$Destination, [string]$EvidenceRoot) {
  $private = [IO.Path]::GetFullPath($Destination)
  $export = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
  if ($private.StartsWith($export, [StringComparison]::OrdinalIgnoreCase) -or $private -eq $export.TrimEnd([IO.Path]::DirectorySeparatorChar)) { throw 'Private profile cannot be inside exported evidence' }
  if ([IO.Path]::GetFullPath($Source) -eq $private) { throw 'Private profile cannot replace source profile' }
  New-Item -ItemType Directory -Path $private | Out-Null
  foreach ($name in @('reaper.ini','reaper-license.rk')) {
    $path = Join-Path $Source $name
    if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination (Join-Path $private $name) }
  }
}
function Assert-EditorEvidence([long]$Hwnd, [bool]$Exists, [bool]$Visible, [bool]$Owned, [bool]$ForegroundBelongs) {
  if ($Hwnd -eq 0 -or -not $Exists) { throw 'Floating editor HWND absent' }
  if (-not $Visible) { throw 'Floating editor is hidden' }
  if (-not $Owned) { throw 'Floating editor belongs to another process' }
  if (-not $ForegroundBelongs) { throw 'Foreground is not the bound editor or its child/popup' }
}
