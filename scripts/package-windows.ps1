# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build the Windows release artefacts: a portable zip and, if WiX is
# available, an MSI.
#
#   pwsh -File scripts/package-windows.ps1                 # zip only
#   pwsh -File scripts/package-windows.ps1 -Msi            # zip + msi
#   pwsh -File scripts/package-windows.ps1 -BuildDir build -DistDir dist
#   pwsh -File scripts/package-windows.ps1 -Msi -Sign      # signed zip + msi
#
# -Sign uses the code-signing certificate in the CurrentUser store (for the
# Certum cloud cert, SimplySign Desktop must be connected first).  With exactly
# one such certificate it is picked automatically; otherwise pass
# -SignThumbprint.  The exe is signed in staging, before it is zipped or packed
# into the MSI, so both carry a signed binary; the MSI is then signed itself.
#
# Everything is staged through `cmake --install`, never by copying from the
# source tree: the working tree contains license.sk, and a packaging script
# that globs is one bad pattern away from shipping it.

[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$DistDir  = "dist",
    [string]$Config   = "Release",
    [switch]$Msi,
    [switch]$Sign,
    [string]$SignThumbprint = "",
    [string]$TimestampUrl   = "http://time.certum.pl"
)

$ErrorActionPreference = "Stop"

function Find-SignTool {
    $cmd = Get-Command signtool -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $found = Get-ChildItem $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq "x64" } |
        Sort-Object { [version]$_.Directory.Parent.Name } -ErrorAction SilentlyContinue |
        Select-Object -Last 1
    if (-not $found) { throw "signtool.exe not found. Install the Windows SDK." }
    return $found.FullName
}

function Resolve-SignCert([string]$thumb) {
    $certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) })
    if ($thumb) {
        $certs = @($certs | Where-Object { $_.Thumbprint -eq $thumb.ToUpper() })
    }
    if ($certs.Count -eq 0) {
        throw "No usable code-signing certificate in CurrentUser\My. Is SimplySign Desktop connected?"
    }
    if ($certs.Count -gt 1) {
        $list = ($certs | ForEach-Object { "  $($_.Thumbprint)  $($_.Subject)" }) -join "`n"
        throw "Several code-signing certificates found; pick one with -SignThumbprint:`n$list"
    }
    return $certs[0]
}

# Sign and immediately verify, so a release can never ship a file whose
# signature or timestamp did not take.  The RFC 3161 timestamp keeps the
# signature valid after the one-year certificate expires.
function Invoke-Sign([string]$tool, [string]$thumb, [string]$file) {
    & $tool sign /sha1 $thumb /fd sha256 /tr $TimestampUrl /td sha256 `
        /d "Vivora" /du "https://vivora.dev" $file
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed for $file" }
    & $tool verify /pa /q $file
    if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $file" }
    Write-Host "signed $file"
}
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

    if ($Sign) {
        $signTool = Find-SignTool
        $cert = Resolve-SignCert $SignThumbprint
        Write-Host "signing with $($cert.Subject) ($($cert.Thumbprint))"
        Invoke-Sign $signTool $cert.Thumbprint (Join-Path $stage "vivora.exe")
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
        if ($Sign) { Invoke-Sign $signTool $cert.Thumbprint $msiPath }
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
