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
