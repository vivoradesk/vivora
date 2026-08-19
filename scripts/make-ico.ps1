# Build icons/win/vivora.ico from the per-size PNGs next to it.
#
# Run once and commit the result; it is NOT a build step. Generating it at
# build time would make an image converter a hard dependency on every Windows
# developer's machine, for an asset that changes about once a year.
#
#   pwsh -File scripts/make-ico.ps1
#
# The container format is simple enough to write directly, which is why there
# is no ImageMagick dependency here:
#   ICONDIR      : u16 reserved=0 | u16 type=1 | u16 count
#   ICONDIRENTRY : u8 w | u8 h | u8 palette | u8 reserved | u16 planes
#                  u16 bpp | u32 bytes | u32 offset          (16 bytes each)
#   then each image's bytes, in order.
#
# Entries are stored PNG-compressed, which Windows has accepted at every size
# since Vista.

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $root "icons\win"
$out    = Join-Path $srcDir "vivora.ico"

# Largest first: some shells pick the first entry that is big enough.
$sizes = @(256, 128, 64, 48, 32, 16)

$images = foreach ($s in $sizes) {
    $p = Join-Path $srcDir "vivora-$s.png"
    if (-not (Test-Path $p)) { throw "missing $p" }
    [pscustomobject]@{ Size = $s; Bytes = [System.IO.File]::ReadAllBytes($p) }
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

$bw.Write([uint16]0)                  # reserved
$bw.Write([uint16]1)                  # type: icon
$bw.Write([uint16]$images.Count)

# Image data starts after the directory.
$offset = 6 + (16 * $images.Count)
foreach ($img in $images) {
    # 256 is encoded as 0 in a single byte -- that is the whole reason the
    # format tops out there.
    $dim = if ($img.Size -ge 256) { 0 } else { $img.Size }
    $bw.Write([byte]$dim)             # width
    $bw.Write([byte]$dim)             # height
    $bw.Write([byte]0)                # palette entries (0 = no palette)
    $bw.Write([byte]0)                # reserved
    $bw.Write([uint16]1)              # colour planes
    $bw.Write([uint16]32)             # bits per pixel
    $bw.Write([uint32]$img.Bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $img.Bytes.Length
}
foreach ($img in $images) { $bw.Write($img.Bytes) }

$bw.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
$bw.Dispose(); $ms.Dispose()

$sizeList = ($images | ForEach-Object { $_.Size }) -join ", "
Write-Host "wrote $out ($($images.Count) frames: $sizeList; $((Get-Item $out).Length) bytes)"
