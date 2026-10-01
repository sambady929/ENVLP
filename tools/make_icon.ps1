# Build src/app/ENVLP.ico from src/app/ENVLP.svg.
#
# Rasterises the SVG at the standard Windows icon sizes with rsvg-convert and
# packs the PNGs into a single PNG-compressed .ico (Vista+). Run from the repo
# root:  powershell -File tools/make_icon.ps1
#
# The .ico is checked in; this script only needs re-running when the SVG
# changes. rsvg-convert comes from MSYS2 (mingw-w64-ucrt-x86_64-librsvg).
[CmdletBinding()]
param(
    [string]$Svg  = "src/app/ENVLP.svg",
    [string]$Out  = "src/app/ENVLP.ico",
    [string]$Tmp  = "build/icon",
    [string]$Rsvg = "C:\msys64\ucrt64\bin\rsvg-convert.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Rsvg)) { throw "rsvg-convert not found at $Rsvg" }
if (-not (Test-Path $Svg))  { throw "SVG not found: $Svg" }

$sizes = @(16, 24, 32, 48, 64, 128, 256)
New-Item -ItemType Directory -Path $Tmp -Force | Out-Null

$pngs = @()
foreach ($s in $sizes) {
    $png = Join-Path $Tmp "icon_$s.png"
    & $Rsvg -w $s -h $s $Svg -o $png
    if ($LASTEXITCODE -ne 0) { throw "rsvg-convert failed for size $s" }
    $pngs += ,@{ Size = $s; Bytes = [System.IO.File]::ReadAllBytes($png) }
}

# --- pack as a PNG-compressed ICO (no BMP fallback; Vista+ understands it) ---
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

# ICONDIR
$bw.Write([UInt16]0)            # reserved
$bw.Write([UInt16]1)            # type: 1 = icon
$bw.Write([UInt16]$pngs.Count)  # image count

# ICONDIRENTRY offset table (16 bytes per entry), then the image data.
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $dim = if ($p.Size -ge 256) { 0 } else { $p.Size }  # 0 encodes 256
    $bw.Write([Byte]$dim)                 # width
    $bw.Write([Byte]$dim)                 # height
    $bw.Write([Byte]0)                    # palette count
    $bw.Write([Byte]0)                    # reserved
    $bw.Write([UInt16]1)                  # colour planes
    $bw.Write([UInt16]32)                 # bits per pixel
    $bw.Write([UInt32]$p.Bytes.Length)    # bytes in resource
    $bw.Write([UInt32]$offset)            # offset
    $offset += $p.Bytes.Length
}
foreach ($p in $pngs) { $bw.Write($p.Bytes) }

$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $root $Out), $ms.ToArray())
$bw.Dispose()

Write-Host ("Wrote {0} ({1} bytes, {2} sizes)" -f $Out,
            (Get-Item $Out).Length, $pngs.Count)
