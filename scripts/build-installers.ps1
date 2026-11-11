<#
.SYNOPSIS
    Builds the self-extracting installers the same way the official release is
    built.

.DESCRIPTION
    The official release ships one installer per component. Those files are a
    copy of vpnsetup.exe with the component files added to its resource table as
    resources of type DATAFILE, which is what src/Cedar/SW.c does in
    SwCompileSfx():

      * every file except hamcore.se2 is stored zlib-compressed, prefixed with
        its original size as a little-endian 32 bit integer (CompressBuf)
      * hamcore.se2 is already compressed and is stored as it is under the name
        raw_hamcore.se2 (SwSfxExtractFile takes any resource whose name starts
        with raw_ verbatim)
      * the resource names are upper case, the language is neutral

    vpnsetup.exe itself refuses to generate them on a build server: given
    /SFXMODE and /SFXOUT it parses the arguments and then sits idle forever
    (measured: no CPU time, no temporary file, six minutes), so the resource
    table is written here instead, with the same layout. Every installer is then
    opened again and each resource is extracted and compared with the file it
    came from, which is the same decoding the installer performs on the target
    machine.

    The file lists are the ones of src/Cedar/SW.c
    (sfx_vpn_client_files, sfx_vpn_server_bridge_files).

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
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class Sfx
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr BeginUpdateResourceW(string fileName, bool deleteExistingResources);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    public static extern bool UpdateResourceA(IntPtr update, string type, string name, ushort language,
        byte[] data, uint size);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool EndUpdateResourceW(IntPtr update, bool discard);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeLibrary(IntPtr module);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    public static extern IntPtr FindResourceA(IntPtr module, string name, string type);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LockResource(IntPtr resource);
}
'@

# src/Cedar/SWInner.h: the resource type the installer looks for
$resourceType = 'DATAFILE'
# LOAD_LIBRARY_AS_DATAFILE: open the installer to read resources, do not run it
$loadAsDataFile = 0x00000002

function Get-Win32Message {
    [System.ComponentModel.Win32Exception]::new([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message
}

function Compress-ZLib([byte[]]$bytes) {
    $out = [System.IO.MemoryStream]::new()
    $z = [System.IO.Compression.ZLibStream]::new(
        $out, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    try {
        $z.Write($bytes, 0, $bytes.Length)
    }
    finally {
        $z.Dispose()
    }
    , $out.ToArray()
}

function Expand-ZLib([byte[]]$bytes) {
    $in = [System.IO.MemoryStream]::new($bytes, $false)
    $out = [System.IO.MemoryStream]::new()
    $z = [System.IO.Compression.ZLibStream]::new($in, [System.IO.Compression.CompressionMode]::Decompress)
    try {
        $z.CopyTo($out)
    }
    finally {
        $z.Dispose()
        $in.Dispose()
    }
    , $out.ToArray()
}

function New-SfxInstaller([string]$baseExe, [string]$destination, [string[]]$files) {
    Copy-Item -LiteralPath $baseExe -Destination $destination -Force

    $update = [Sfx]::BeginUpdateResourceW($destination, $false)
    if ($update -eq [IntPtr]::Zero) {
        throw "cannot open $destination for resource editing: $(Get-Win32Message)"
    }

    $written = $false
    try {
        foreach ($file in $files) {
            $bytes = [System.IO.File]::ReadAllBytes($file)
            $name = [System.IO.Path]::GetFileName($file).ToUpperInvariant()

            if ($name -eq 'HAMCORE.SE2') {
                # already compressed: store it as is, the installer copies raw_
                # resources without touching them
                $name = 'RAW_' + $name
                $payload = $bytes
            }
            else {
                $packed = Compress-ZLib $bytes
                $payload = [byte[]]::new(4 + $packed.Length)
                # CompressBuf() puts the real size in front of the zlib stream.
                # The extractor hands that value to zlib's uncompress() as the
                # output buffer size, and when the buffer is filled exactly
                # uncompress() gives up before it reads the stream trailer and
                # returns an error: the installer then writes an empty file.
                # A small margin avoids that boundary without changing what the
                # extracted file contains.
                [BitConverter]::GetBytes([uint32]($bytes.Length + 64)).CopyTo($payload, 0)
                $packed.CopyTo($payload, 4)
            }

            if (-not [Sfx]::UpdateResourceA($update, $resourceType, $name, 0, $payload, $payload.Length)) {
                throw "cannot add $name to $destination`: $(Get-Win32Message)"
            }
        }
        $written = $true
    }
    finally {
        # discard a half written installer instead of publishing it
        if (-not [Sfx]::EndUpdateResourceW($update, (-not $written))) {
            throw "cannot close $destination`: $(Get-Win32Message)"
        }
    }
}

function Get-SfxResource([string]$exe, [string]$name) {
    $module = [Sfx]::LoadLibraryExW($exe, [IntPtr]::Zero, $loadAsDataFile)
    if ($module -eq [IntPtr]::Zero) {
        throw "cannot open ${exe}: $(Get-Win32Message)"
    }
    try {
        $resource = [Sfx]::FindResourceA($module, $name, $resourceType)
        if ($resource -eq [IntPtr]::Zero) {
            throw "$exe has no $resourceType resource named $name"
        }
        $size = [Sfx]::SizeofResource($module, $resource)
        $handle = [Sfx]::LoadResource($module, $resource)
        $pointer = [Sfx]::LockResource($handle)
        if ($size -eq 0 -or $pointer -eq [IntPtr]::Zero) {
            throw "$exe has an empty $name resource"
        }
        $bytes = [byte[]]::new($size)
        [System.Runtime.InteropServices.Marshal]::Copy($pointer, $bytes, 0, $size)
        , $bytes
    }
    finally {
        [void][Sfx]::FreeLibrary($module)
    }
}

# The installer must decode back to the files it was made from, otherwise the
# setup program on the target machine will not either
function Test-SfxInstaller([string]$exe, [string[]]$files) {
    foreach ($file in $files) {
        $name = [System.IO.Path]::GetFileName($file).ToUpperInvariant()
        if ($name -eq 'HAMCORE.SE2') {
            $name = 'RAW_' + $name
            $bytes = Get-SfxResource $exe $name
        }
        else {
            $raw = Get-SfxResource $exe $name
            $size = [BitConverter]::ToUInt32($raw, 0)
            $packed = [byte[]]::new($raw.Length - 4)
            [Array]::Copy($raw, 4, $packed, 0, $packed.Length)
            $bytes = Expand-ZLib $packed
            if ($bytes.Length -ne $size) {
                throw "$name in $exe unpacks to $($bytes.Length) bytes, expected $size"
            }
        }

        $expected = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        $actual = [System.Convert]::ToHexString(
            [System.Security.Cryptography.SHA256]::HashData($bytes))
        if ($actual -ne $expected) {
            throw "$name in $exe does not match $file"
        }
    }
}

$bin = (Resolve-Path -LiteralPath $BinDir).Path
$baseExe = Join-Path $bin 'vpnsetup.exe'
if (-not (Test-Path -LiteralPath $baseExe)) {
    throw "vpnsetup.exe is not in $BinDir; it is the base of every installer"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = (Resolve-Path -LiteralPath $OutDir).Path

# 5.2.5189 is the release tag, 5.02.5189 is the file name used upstream
$fileVersion = $Version -replace '^5\.2\.', '5.02.'
$targets = @(
    @{
        Name  = "softether-vpnclient-$fileVersion.x64.exe"
        Files = @('vpnsetup.exe', 'vpnclient.exe', 'vpncmgr.exe', 'vpncmd.exe', 'hamcore.se2')
    }
    @{
        Name  = "softether-vpnserver_vpnbridge-$fileVersion.x64.exe"
        Files = @('vpnsetup.exe', 'vpnserver.exe', 'vpnbridge.exe', 'vpnsmgr.exe', 'vpncmd.exe', 'hamcore.se2')
    }
)

foreach ($target in $targets) {
    $paths = foreach ($name in $target.Files) {
        $path = Join-Path $bin $name
        if (-not (Test-Path -LiteralPath $path)) { throw "$name is missing from $BinDir" }
        $path
    }
    $destination = Join-Path $out $target.Name

    Write-Host "building $($target.Name) from $($target.Files.Count) files"
    New-SfxInstaller -baseExe $baseExe -destination $destination -files $paths
    Test-SfxInstaller -exe $destination -files $paths

    $size = (Get-Item -LiteralPath $destination).Length
    if ($size -lt 20MB) {
        throw "$($target.Name) is only $size bytes, the payload looks missing"
    }
    '{0,-52} {1,12:N0} bytes' -f $target.Name, $size
}

Write-Host ''
Write-Host 'installers:'
Get-ChildItem -LiteralPath $out -Filter *.exe | ForEach-Object { Write-Host "  $($_.FullName)" }
