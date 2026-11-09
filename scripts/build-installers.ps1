<#
.SYNOPSIS
    Builds the self-extracting installers the same way the official release is
    built.

.DESCRIPTION
    vpnsetup.exe carries the installer generator of the official build. Called
    with /SFXMODE and /SFXOUT it copies itself, embeds the files of the chosen
    component as resources, compresses everything except hamcore.se2 (that one
    is already compressed and is stored as raw_hamcore.se2) and writes the
    result: one .exe that installs in place, exactly like
    softether-vpnclient-*.x64.exe from the upstream release.

    The component file lists are the ones in src/Cedar/SW.c
    (sfx_vpn_client_files, sfx_vpn_server_bridge_files), so vpnsetup.exe reads
    them from its own directory; -BinDir must therefore be the directory that
    holds the built tools.

.EXAMPLE
    ./scripts/build-installers.ps1 -Version 5.2.5189 -BinDir out -OutDir dist
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Version,
    [Parameter(Mandatory)] [string]$BinDir,
    [Parameter(Mandatory)] [string]$OutDir
)

$ErrorActionPreference = 'Stop'

$setup = Join-Path (Resolve-Path -LiteralPath $BinDir).Path 'vpnsetup.exe'
if (-not (Test-Path -LiteralPath $setup)) {
    throw "vpnsetup.exe is not in $BinDir; it is the installer generator"
}

# 5.2.5189 is the release tag, 5.02.5189 is the file name used upstream
$fileVersion = $Version -replace '^5\.2\.', '5.02.'
$targets = @(
    @{ Mode = 'vpnclient'; Name = "softether-vpnclient-$fileVersion.x64.exe" }
    @{ Mode = 'vpnserver_vpnbridge'; Name = "softether-vpnserver_vpnbridge-$fileVersion.x64.exe" }
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$written = @()

foreach ($target in $targets) {
    $out = Join-Path (Resolve-Path -LiteralPath $OutDir).Path $target.Name
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue

    Write-Host "building $($target.Name) (SFXMODE=$($target.Mode))"
    # vpnsetup.exe is a GUI binary: the working directory is the tool directory
    # because the generator copies "vpnsetup.exe" by a relative name and reads
    # the payload from its own directory. Pass the arguments as an array, one
    # element per argument, so that PowerShell quotes them itself.
    $arguments = @("/SFXMODE:$($target.Mode)", "/SFXOUT:$out")
    $process = Start-Process -FilePath $setup -ArgumentList $arguments `
        -WorkingDirectory (Split-Path -Parent $setup) -PassThru
    # if the arguments did not arrive, vpnsetup.exe opens its setup wizard and
    # waits for a click that a build server never gives it
    if (-not $process.WaitForExit(180000)) {
        $process.Kill()
        throw "vpnsetup.exe did not finish within three minutes for $($target.Mode)"
    }
    if ($process.ExitCode -ne 0) {
        throw "vpnsetup.exe exited with $($process.ExitCode) for $($target.Mode)"
    }
    if (-not (Test-Path -LiteralPath $out)) {
        throw "$($target.Name) was not created"
    }

    # the payload lives in the resource table, whose names are UTF-16
    $text = [System.Text.Encoding]::Unicode.GetString([System.IO.File]::ReadAllBytes($out))
    foreach ($needle in @('RAW_HAMCORE.SE2', 'VPNCMD.EXE')) {
        if (-not $text.Contains($needle)) {
            throw "$($target.Name) does not carry $needle; the payload was not embedded"
        }
    }

    $size = (Get-Item -LiteralPath $out).Length
    if ($size -lt 20MB) {
        throw "$($target.Name) is only $size bytes, the payload looks missing"
    }

    '{0,-52} {1,12:N0} bytes' -f $target.Name, $size
    $written += $out
}

Write-Host ''
Write-Host 'installers:'
$written | ForEach-Object { Write-Host "  $_" }
