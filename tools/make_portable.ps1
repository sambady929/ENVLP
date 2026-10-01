# Assemble a self-contained, no-install Windows package for ENVLP.
#
#   powershell -ExecutionPolicy Bypass -File tools/make_portable.ps1
#
# Produces dist/ENVLP-<version>-win64/ containing ENVLP.exe, every runtime
# DLL it needs (found transitively with objdump), the example circuits and a
# README. Copy that folder anywhere (including a USB stick) and run ENVLP.exe
# -- nothing needs to be installed.
#
# The MinGW runtime DLLs come from the MSYS2 UCRT64 tree; the list is computed,
# not hard-coded, so it stays correct if a dependency changes.
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$DistDir  = "dist",
    [string]$MsysBin  = "C:\msys64\ucrt64\bin",
    [string]$Objdump  = "C:\msys64\ucrt64\bin\objdump.exe",
    [string]$Strip    = "C:\msys64\ucrt64\bin\strip.exe",
    [string]$Config   = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Objdump)) { throw "objdump not found at $Objdump" }

# --- build ------------------------------------------------------------------
# Single-config Ninja: set the build type at configure time and do NOT pass
# --config to the build step (that is a multi-config/Visual Studio concept).
$buildTypeArg = "-DCMAKE_BUILD_TYPE=" + $Config
cmake -S . -B $BuildDir $buildTypeArg
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $BuildDir "bin/ENVLP.exe"
if (-not (Test-Path $exe)) { throw "ENVLP.exe not found at $exe" }

# --- version ----------------------------------------------------------------
$ver = (Select-String -Path CMakeLists.txt -Pattern 'project\(ENVLP VERSION\s+([0-9.]+)').Matches[0].Groups[1].Value
if (-not $ver) { $ver = "0.0.0" }
$outDir = Join-Path $DistDir "ENVLP-$ver-win64"
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# --- resolve the DLL closure ------------------------------------------------
function Get-Imports([string]$path) {
    & $Objdump -p $path 2>$null |
        Select-String "DLL Name" |
        ForEach-Object { ($_ -replace '.*DLL Name:\s*', '').Trim() }
}

$seen  = @{}
$queue = New-Object System.Collections.Queue
foreach ($d in Get-Imports $exe) { $queue.Enqueue($d) }
while ($queue.Count -gt 0) {
    $d = $queue.Dequeue()
    if ($seen.ContainsKey($d)) { continue }
    $seen[$d] = $true
    $f = Join-Path $MsysBin $d
    if (Test-Path $f) {
        foreach ($x in Get-Imports $f) {
            if (-not $seen.ContainsKey($x)) { $queue.Enqueue($x) }
        }
    }
}

# Only ship DLLs that actually live in the MSYS2 tree; the rest are Windows
# system DLLs that are always present.
$bundled = $seen.Keys | Where-Object { Test-Path (Join-Path $MsysBin $_) } |
           Sort-Object

Copy-Item $exe $outDir
# Strip the debug/symbol tables: the executable is ~85 MB unstripped because
# GiNaC/CLN are linked in statically, and ~5 MB after stripping.
if (Test-Path $Strip) {
    & $Strip (Join-Path $outDir "ENVLP.exe")
    if ($LASTEXITCODE -ne 0) { Write-Warning "strip failed; shipping unstripped" }
}
foreach ($d in $bundled) { Copy-Item (Join-Path $MsysBin $d) $outDir }

# --- example circuits + a short readme --------------------------------------
if (Test-Path "examples") {
    Copy-Item "examples" (Join-Path $outDir "examples") -Recurse
}

$readme = @"
ENVLP $ver -- portable Windows build (64-bit)
=============================================

Run ENVLP.exe. Nothing needs to be installed: the compiler runtime and the
GUI/maths libraries ship in this folder. You can copy it to a USB stick.

  ENVLP.exe        the program
  *.dll            bundled runtimes (wxWidgets, GiNaC, CLN/GMP, Lua, MinGW)
  examples/        example schematics (*.scx)

Notes
-----
* The typeset "Results" tab uses the Microsoft Edge WebView2 runtime, which
  is already present on Windows 10/11. On a machine without it, use the
  "Results (Text)" tab; everything else works normally.
* Circuit files use the .scx text format.
"@
Set-Content -Path (Join-Path $outDir "README.txt") -Value $readme

$size = (Get-ChildItem $outDir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Wrote {0}  ({1} files, {2:N1} MB)" -f $outDir,
            (Get-ChildItem $outDir -Recurse -File).Count, ($size / 1MB))
