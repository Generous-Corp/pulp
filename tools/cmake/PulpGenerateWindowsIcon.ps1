param(
    [Parameter(Mandatory = $true)][string]$InputPng,
    [Parameter(Mandatory = $true)][string]$OutputIco,
    [Parameter(Mandatory = $true)][string]$RcPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

function Resize-ToPngBytes {
    param(
        [System.Drawing.Image]$Image,
        [int]$Size
    )

    $bitmap = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.DrawImage($Image, 0, 0, $Size, $Size)

    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)

    $graphics.Dispose()
    $bitmap.Dispose()

    return ,@($stream.ToArray())
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$source = [System.Drawing.Image]::FromFile($InputPng)

try {
    $frames = @()
    foreach ($size in $sizes) {
        $frames += ,@($size, (Resize-ToPngBytes -Image $source -Size $size))
    }

    $outDir = Split-Path -Parent $OutputIco
    if (-not [string]::IsNullOrWhiteSpace($outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }

    $writer = New-Object System.IO.BinaryWriter([System.IO.File]::Open($OutputIco, [System.IO.FileMode]::Create))
    try {
        $writer.Write([UInt16]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]$frames.Count)

        $offset = 6 + (16 * $frames.Count)
        foreach ($frame in $frames) {
            $size = [int]$frame[0]
            $bytes = [byte[]]$frame[1]
            $dimension = if ($size -ge 256) { 0 } else { $size }
            $writer.Write([byte]$dimension)
            $writer.Write([byte]$dimension)
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]32)
            $writer.Write([UInt32]$bytes.Length)
            $writer.Write([UInt32]$offset)
            $offset += $bytes.Length
        }

        foreach ($frame in $frames) {
            $writer.Write([byte[]]$frame[1])
        }
    } finally {
        $writer.Dispose()
    }

    $escaped = ($OutputIco -replace '\\', '\\')
    "IDI_ICON1 ICON `"$escaped`"" | Set-Content -NoNewline -Encoding ASCII -Path $RcPath
} finally {
    $source.Dispose()
}
