$ErrorActionPreference = 'Stop'

# Run only after the offline host-side qcow2 expansion and a clean guest boot.
# DiskPart is deliberately not used here: Resize-Partition reports the actual
# supported maximum and fails closed if C: cannot consume the new capacity.
$partition = Get-Partition -DriveLetter C
$limits = Get-PartitionSupportedSize -DriveLetter C
if ($limits.SizeMax -le $partition.Size) {
    throw "C: has no additional supported capacity (current=$($partition.Size), max=$($limits.SizeMax))"
}
Resize-Partition -DriveLetter C -Size $limits.SizeMax
$volume = Get-Volume -DriveLetter C
[pscustomobject]@{
    drive = 'C:'
    size_bytes = $volume.Size
    free_bytes = $volume.SizeRemaining
    extended = $true
} | ConvertTo-Json -Compress
