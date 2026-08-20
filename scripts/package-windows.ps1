# Build the Windows release artefacts: a portable zip and, if WiX is
# available, an MSI.
#
#   pwsh -File scripts/package-windows.ps1                 # zip only
#   pwsh -File scripts/package-windows.ps1 -Msi            # zip + msi
#   pwsh -File scripts/package-windows.ps1 -BuildDir build -DistDir dist
#
# Everything is staged through `cmake --install`, never by copying from the
# source tree: the working tree contains license.sk, and a packaging script
# that globs is one bad pattern away from shipping it.

[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$DistDir  = "dist",
    [string]$Config   = "Release",
    [switch]$Msi
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $envFile = Join-Path $BuildDir "artefact.env"
    if (-not (Test-Path $envFile)) {
        throw "$envFile not found. Configure the build first: cmake -B $BuildDir"
    }

    # CMake decides what a release is called; we only read it.
    $vars = @{}
    foreach ($line in Get-Content $envFile) {
        if ($line -match '^([A-Z_]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] }
    }
    $base    = $vars["VIVORA_ARTEFACT_BASENAME"]     # Vivora-0.1.0-windows-x64
    $version = $vars["VIVORA_VERSION"]               # 0.1.0
    if (-not $base -or -not $version) { throw "artefact.env is missing keys" }

    $stage = Join-Path (Join-Path $BuildDir "stage") $base
    Write-Host "staging $base -> $stage"
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $stage | Out-Null

    cmake --install $BuildDir --config $Config --component app --prefix $stage
    if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }

    Copy-Item (Join-Path $root "packaging\windows\README-portable.txt") $stage
    Copy-Item (Join-Path $root "packaging\THIRD-PARTY-NOTICES.md")      $stage

    # Vivora is one self-contained exe since VIV-122; a stray DLL beside it
    # would mean the OpenSSL TLS plugin crept back into the link.
    foreach ($required in @("vivora.exe")) {
        if (-not (Test-Path (Join-Path $stage $required))) {
            throw "staging is missing $required"
        }
    }

    New-Item -ItemType Directory -Force $DistDir | Out-Null
    $zip = Join-Path $DistDir "$base.zip"
    Remove-Item $zip -ErrorAction SilentlyContinue
    Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "wrote $zip"

    $artefacts = @($zip)

    if ($Msi) {
        $wix = Get-Command wix -ErrorAction SilentlyContinue
        if (-not $wix) {
            throw "wix not found. Install it with: dotnet tool install --global wix"
        }
        $msiPath = Join-Path $DistDir "$base.msi"
        Remove-Item $msiPath -ErrorAction SilentlyContinue
        # MSI compares only the first three numeric fields of ProductVersion and
        # rejects anything non-numeric, so a -rc1 suffix lives in the FILE name
        # and never in -d Version.
        # Argument array rather than backtick continuations: a stray space
        # after a backtick silently ends the statement and the remaining
        # lines get bound to this script's own parameters instead.
        $wixArgs = @(
            "build", (Join-Path $root "packaging\windows\vivora.wxs"),
            "-arch", "x64",
            "-d", "Version=$version",
            "-b", "stage=$stage",
            "-b", "icons=$(Join-Path $root 'icons\win')",
            "-o", $msiPath
        )
        & wix @wixArgs
        if ($LASTEXITCODE -ne 0) { throw "wix build failed" }
        Write-Host "wrote $msiPath"
        $artefacts += $msiPath
    }

    # One checksum file per dist directory, appended to so a Linux artefact
    # built elsewhere can join the same list.
    $sums = Join-Path $DistDir "SHA256SUMS.txt"
    foreach ($a in $artefacts) {
        $h = (Get-FileHash $a -Algorithm SHA256).Hash.ToLower()
        $line = "$h  $(Split-Path -Leaf $a)"
        # Replace any previous line for the same file rather than duplicating.
        $existing = if (Test-Path $sums) {
            Get-Content $sums | Where-Object { $_ -notmatch [regex]::Escape((Split-Path -Leaf $a)) + '$' }
        } else { @() }
        Set-Content -Path $sums -Value (@($existing) + $line) -Encoding ascii
        Write-Host $line
    }
}
finally {
    Pop-Location
}
