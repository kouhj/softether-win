SoftEther VPN {{VERSION}}, built from the official upstream release with MSVC
(x64, static CRT) plus one local patch and one local feature.

## Installers

`softether-vpnclient-{{FILE_VERSION}}.x64.exe` and
`softether-vpnserver_vpnbridge-{{FILE_VERSION}}.x64.exe` are self-extracting
installers with the layout of the official release: a copy of `vpnsetup.exe`
with the component files in its `DATAFILE` resource table, everything except
`hamcore.se2` zlib compressed behind its original size, and `hamcore.se2`
stored untouched as `raw_hamcore.se2`.

`vpnsetup.exe` cannot build them on a build server: given `/SFXMODE` and
`/SFXOUT` it parses the arguments and then sits idle, six minutes with 0.2 s of
CPU time and no temporary file. `scripts/build-installers.ps1` therefore writes
the same resource table itself, then opens every installer again, decodes each
resource and compares it with the file it came from, which is the decoding the
installer performs on the target machine.

Run the one you need and the usual setup wizard installs the VPN Client, or the
VPN Server and VPN Bridge together.

## Archive

`softether-win-{{VERSION}}-x64.zip` holds `vpncmgr.exe`, `vpnclient.exe`,
`vpnserver.exe`, `vpnbridge.exe`, `vpncmd.exe`, `vpnsetup.exe`, `vpnsmgr.exe`
and `hamcore.se2`; unpack it over an existing installation of the same version
to upgrade it.

To install or upgrade from the archive, open PowerShell as administrator and
run:

    .\install.ps1                     upgrade what is installed
    .\install.ps1 -Component server   fresh server installation
    .\install.ps1 -NoSetup            files only, run vpnsetup.exe yourself

The script checks the files against `SHA256SUMS`, stops the VPN processes
(`-Force`), copies the files and runs `vpnsetup.exe`, which registers the
drivers and the services.

## Patch and feature

Patch: the two assignments of `is_restricted` in `src/Cedar/Server.c` are
commented out, so the enterprise feature flags (push route, RADIUS, certificate
auth, deep logging, access control, syslog) stay enabled.

Feature: network connection rules, a tray menu entry (*Network connection
rules...*) in `vpncmgr.exe` that connects or disconnects a VPN Client
connection depending on the Wi-Fi SSID or the Ethernet default gateway. Rules
live in `%ProgramData%\SoftEther VPN Client\net_rules.ini`.

Not verified on a real desktop yet: see the smoke test in the README.
