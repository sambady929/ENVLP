# Authenticode-sign the ENVLP executable with osslsigncode.
#
#   powershell -ExecutionPolicy Bypass -File tools/sign.ps1 `
#       -Pfx mycert.pfx -Password 'secret'
#
# or with a PEM certificate + key instead of a PFX:
#
#   powershell -ExecutionPolicy Bypass -File tools/sign.ps1 `
#       -Cert mycert.pem -Key mykey.pem
#
# WHAT THIS IS FOR
# ----------------
# An unsigned executable downloaded from the internet triggers Windows
# SmartScreen ("Windows protected your PC"). Signing with a code-signing
# certificate that chains to a publicly trusted CA is what removes it:
#
#   * an EV (extended-validation) certificate is trusted immediately;
#   * an OV (organization-validation) certificate builds SmartScreen
#     reputation as the download count grows.
#
# A *self-signed* certificate does NOT help other people: they would have to
# install it into their Trusted Root store first. It is only useful for your
# own machines / an internal deployment.
#
# The script signs ENVLP.exe in build/bin and every dist/ENVLP-*-win64 folder.
[CmdletBinding()]
param(
    # A .pfx (PKCS#12) holding the certificate and its private key.
    [string]$Pfx,
    [string]$Password,
    # Or a PEM certificate + PEM/der private key.
    [string]$Cert,
    [string]$Key,
    # RFC3161 timestamp server (so the signature outlives the certificate).
    [string]$Timestamp = "http://timestamp.digicert.com",
    [string]$Osslsign  = "C:\msys64\ucrt64\bin\osslsigncode.exe",
    # Extra files to sign (defaults to every ENVLP.exe under build/ and dist/).
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Osslsign)) {
    throw "osslsigncode not found at $Osslsign. Install it with:`n" +
          "  pacman -S mingw-w64-ucrt-x86_64-osslsigncode"
}

if (-not $Pfx -and -not $Cert) {
    throw "Supply either -Pfx (with -Password) or -Cert (with -Key)."
}
if ($Pfx -and -not (Test-Path $Pfx)) { throw "PFX not found: $Pfx" }
if ($Cert -and -not (Test-Path $Cert)) { throw "cert not found: $Cert" }
if ($Cert -and -not $Key) { throw "-Key is required with -Cert." }

# Default target list: the built exe plus every packaged copy.
if (-not $Files) {
    $Files = @()
    foreach ($p in @("build/bin/ENVLP.exe")) {
        if (Test-Path $p) { $Files += $p }
    }
    $Files += (Get-ChildItem dist -Recurse -Filter ENVLP.exe -ErrorAction SilentlyContinue |
               ForEach-Object { $_.FullName })
}
if ($Files.Count -eq 0) { throw "No ENVLP.exe found to sign (build first)." }

# Common arguments; the key material is passed per file. Build a plain array
# and splat it (avoid passing an empty string for -ts, which osslsigncode
# rejects).
$base = @("sign", "-h", "sha2", "-n", "ENVLP",
          "-i", "https://github.com/sambady929/ENVLP")
if ($Timestamp.Trim()) { $base += @("-ts", $Timestamp.Trim()) }
if ($Pfx) {
    $base += @("-pkcs12", (Resolve-Path $Pfx).Path)
    if ($Password) { $base += @("-pass", $Password) }
} else {
    $base += @("-certs", (Resolve-Path $Cert).Path, "-key", (Resolve-Path $Key).Path)
}

foreach ($f in $Files) {
    $full  = (Resolve-Path $f).Path
    $signed = "$full.signed"
    if (Test-Path $signed) { Remove-Item $signed -Force }

    Write-Host "Signing $full"
    $args = $base + @("-in", $full, "-out", $signed)
    & $Osslsign @args
    if ($LASTEXITCODE -ne 0) { throw "osslsigncode failed for $full" }

    # Replace only after a successful signature so a failure never leaves the
    # tree without a working exe.
    Move-Item -Force $signed $full

    $sig = Get-AuthenticodeSignature $full
    Write-Host ("  -> {0}: {1}" -f (Split-Path -Leaf $full), $sig.Status)
}

Write-Host "`nDone. Verify on a fresh machine: the file should show a" -NoNewline
Write-Host " Digital Signatures tab in its Properties."
