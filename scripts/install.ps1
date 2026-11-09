<#
.SYNOPSIS
    Installs or upgrades the patched SoftEther VPN build.

.DESCRIPTION
    The release archive holds the eight files of a SoftEther VPN Windows
    installation: vpnsetup.exe, vpnsmgr.exe, vpncmgr.exe, vpnclient.exe,
    vpnserver.exe, vpnbridge.exe, vpncmd.exe and hamcore.se2.

    This script puts them where an existing installation lives (or in
    %ProgramFiles%\SoftEther VPN Client for a fresh one) and then runs
    vpnsetup.exe, which installs the drivers and the services. It verifies the
    archive against SHA256SUMS from the same release, refuses to run without
    administrator rights and refuses to kill a running VPN client unless -Force
    is given.

.EXAMPLE
    # install or upgrade the default component, running the setup program
    .\install.ps1

.EXAMPLE
    # only replace the files, run vpnsetup.exe yourself afterwards
    .\install.ps1 -NoSetup

.EXAMPLE
    # upgrade a server installation from a zip that is already on disk
    .\install.ps1 -Component server -Zip .\softether-win-5.2.5189-x64.zip
#>
[CmdletBinding()]
param(
    [string]$Version = '5.2.5189',

    # Path of an already downloaded softether-win-<version>-x64.zip. Without it
    # the archive is downloaded from this repository's release.
    [string]$Zip,

    # Install into this directory instead of the one that is detected.
    [string]$InstallDir,

    [ValidateSet('client', 'server', 'bridge')]
    [string]$Component = 'client',

    # Copy the files but do not start vpnsetup.exe.
    [switch]$NoSetup,

    # Stop running VPN processes without asking.
    [switch]$Force,

    # Do not check the archive against SHA256SUMS.
    [switch]$SkipHashCheck,

    # Print what would happen and change nothing.
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$asset = "softether-win-$Version-x64.zip"
$releaseUrl = "https://github.com/kouhj/softether-win/releases/download/$Version"
$processNames = @('vpncmgr', 'vpnclient', 'vpnserver', 'vpnbridge', 'vpncmd')
$componentDirs = @{
    client = 'SoftEther VPN Client'
    server = 'SoftEther VPN Server'
    bridge = 'SoftEther VPN Bridge'
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-InstallDirs {
    # every SoftEther installation that is present, whatever the component
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
    $found = foreach ($root in $roots) {
        foreach ($name in $componentDirs.Values) {
            $dir = Join-Path $root $name
            if (Test-Path -LiteralPath (Join-Path $dir 'hamcore.se2')) { $dir }
        }
    }
    return @($found | Select-Object -Unique)
}

function Get-Archive {
    if ($Zip) {
        $path = (Resolve-Path -LiteralPath $Zip).Path
        Write-Host "using $path"
        return $path
    }

    $destination = Join-Path ([IO.Path]::GetTempPath()) $asset
    if ((Test-Path -LiteralPath $destination) -and -not $SkipHashCheck) {
        Write-Host "reusing $destination"
        return $destination
    }

    Write-Host "downloading $releaseUrl/$asset"
    Invoke-WebRequest -Uri "$releaseUrl/$asset" -OutFile $destination -UseBasicParsing
    return $destination
}

function Assert-Checksum([string]$Path) {
    if ($SkipHashCheck) {
        Write-Warning 'the checksum was not verified (-SkipHashCheck)'
        return
    }

    $sums = Join-Path ([IO.Path]::GetTempPath()) "softether-$Version-SHA256SUMS"
    Invoke-WebRequest -Uri "$releaseUrl/SHA256SUMS" -OutFile $sums -UseBasicParsing
    $line = Get-Content -LiteralPath $sums | Where-Object { $_ -match [regex]::Escape($asset) } | Select-Object -First 1
    if (-not $line) { throw "SHA256SUMS does not list $asset" }

    $expected = ($line -split '\s+')[0].ToLowerInvariant()
    $actual = Get-Sha256 $Path
    if ($expected -ne $actual) {
        throw "checksum mismatch: SHA256SUMS says $expected, the archive is $actual"
    }
    Write-Host "sha256 $actual matches SHA256SUMS"
}

# --------------------------------------------------------------------------
# what to do
# --------------------------------------------------------------------------

if (-not $DryRun -and -not (Test-IsAdmin)) {
    throw 'this script must run as administrator (right click PowerShell -> Run as administrator)'
}

$targets = if ($InstallDir) {
    @($InstallDir)
} else {
    $existing = Get-InstallDirs
    if ($existing.Count -gt 0) {
        $existing
    } else {
        @(Join-Path $env:ProgramFiles $componentDirs[$Component])
    }
}

Write-Host "version:    $Version"
Write-Host "components: $($targets -join ', ')"

$running = @(Get-Process -Name $processNames -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $names = ($running | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
    if (-not $Force) {
        throw "$names still running; close them or pass -Force to stop them"
    }
    Write-Host "stopping $names"
    if (-not $DryRun) {
        $running | Stop-Process -Force
        Start-Sleep -Seconds 1
    }
}

if ($DryRun) {
    Write-Host 'dry run: nothing was downloaded, copied or started'
    return
}

$archive = Get-Archive
Assert-Checksum $archive

$work = Join-Path ([IO.Path]::GetTempPath()) ("softether-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
Expand-Archive -LiteralPath $archive -DestinationPath $work
$source = Join-Path $work "softether-win-$Version-x64"
if (-not (Test-Path -LiteralPath $source)) {
    $source = (Get-ChildItem -LiteralPath $work -Directory | Select-Object -First 1).FullName
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'hamcore.se2'))) {
    throw "the archive does not contain hamcore.se2 (looked in $source)"
}

foreach ($target in $targets) {
    if (-not (Test-Path -LiteralPath $target)) {
        Write-Host "creating $target"
        New-Item -ItemType Directory -Path $target -Force | Out-Null
    }
    Get-ChildItem -LiteralPath $source -File | Copy-Item -Destination $target -Force
    Write-Host "copied $((Get-ChildItem -LiteralPath $source -File).Count) files to $target"
}

Remove-Item -LiteralPath $work -Recurse -Force

if ($NoSetup) {
    Write-Host ''
    Write-Host 'run the setup program of each installation to register the drivers and services:'
    foreach ($target in $targets) { Write-Host "  & '$target\vpnsetup.exe'" }
    return
}

foreach ($target in $targets) {
    $setup = Join-Path $target 'vpnsetup.exe'
    Write-Host "starting $setup"
    $process = Start-Process -FilePath $setup -WorkingDirectory $target -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "$setup exited with $($process.ExitCode)"
    }
}

Write-Host ''
Write-Host 'done: the installation is updated and the setup program has finished'
