# softether-win

Builds the **official SoftEther VPN** Windows binaries from the upstream release
with two local changes, using GitHub Actions (`windows-latest`, MSVC + vcpkg).

## 1. Enterprise restrictions disabled

`patches/0001-comment-out-is_restricted-assignments.patch` comments out the two
assignments of `is_restricted` in `src/Cedar/Server.c`, so the flags that disable
the "enterprise" features of the open source build (push route, RADIUS, client
certificates, deep logging, access control, syslog) are never set. Same patch as
the `softether-deb` repository.

## 2. Connect / disconnect by network status (VPN Client Manager)

New feature in the client manager (`vpncmgr`), implemented in
`src/Cedar/netrule.c`:

- rules are stored in `%ProgramData%\SoftEther VPN Client\net_rules.ini`;
- a **wireless** rule names one or more SSIDs; it matches when Windows is
  connected to one of them (`WlanQueryInterface` on the current connection);
- a **wired** rule names a default gateway; it matches when the default route
  of an *Ethernet* adapter uses that gateway (`GetIpForwardTable` + `GetIfEntry`);
- a rule then **connects** or **disconnects** either a named connection or
  every connection;
- the monitor re-evaluates all rules every 5 seconds and only acts when the
  state differs, so it is idempotent;
- the GUI (tray menu → *Network connection rules...*) lists the rules and edits
  the fields of the selected one: enable, SSIDs (with *Use current*), wired
  gateway (with *Use current*), action, and the connection name or *All
  connections*.

Note: SSIDs are compared with the SSID Windows currently associates with; the
editor fills the field from the live connection, so no WLAN profile parsing is
needed.

## Installing

A release carries `softether-win-<version>-x64.zip`, `install.ps1` and
`SHA256SUMS`. The zip is the eight files of an installation
(`vpnsetup.exe`, `vpnsmgr.exe`, `vpncmgr.exe`, `vpnclient.exe`,
`vpnserver.exe`, `vpnbridge.exe`, `vpncmd.exe`, `hamcore.se2`), which is exactly
what an in-place upgrade of an existing installation replaces.

`install.ps1` automates that upgrade. Run it from an **administrator**
PowerShell:

```powershell
.\install.ps1                      # upgrade every installation that is present
.\install.ps1 -Component server    # fresh server installation
.\install.ps1 -NoSetup             # copy the files, run vpnsetup.exe yourself
.\install.ps1 -Zip .\softether-win-5.2.5189-x64.zip
.\install.ps1 -DryRun              # say what would happen, change nothing
```

It downloads the archive from the release unless `-Zip` names a local copy,
checks it against `SHA256SUMS`, refuses to run without administrator rights,
refuses to kill a running VPN client unless `-Force` is given, copies the files
into `%ProgramFiles%\SoftEther VPN Client|Server|Bridge` and finally runs
`vpnsetup.exe`, the official setup program, which installs the drivers and
registers the services. Without a script it is the same manual procedure: unpack
the zip over the installation directory and run `vpnsetup.exe` there.

## How the build works

`scripts/prepare-source.py` downloads `SoftEtherVPN-<version>.tar.xz`, applies
`patches/*.patch` and injects the feature:

| file | change |
|---|---|
| `src/Cedar/netrule.c`, `netrule.h` | copied in verbatim |
| `src/Cedar/CM.c` | includes the header, adds the tray menu entry and its handler, starts/stops the monitor around the message loop, and lets the monitor connect/disconnect without prompting |
| `src/Cedar/CMakeLists.txt` | links `wlanapi` and `iphlpapi` |

Every edit is anchored on an exact string and asserted, so an upstream change
fails the build instead of silently producing a broken tree.

The workflow then configures with CMake + vcpkg (`openssl`, `libsodium`, `zlib`
from the upstream `vcpkg.json`), builds with MSVC and uploads the `*.exe` and
`*.se2` files.

`-DOQS_ENABLE=OFF` is passed on purpose: the vendored `oqs-provider` fails to
compile with recent MSVC (`error C7757: an array of unknown size cannot be
initialized by an empty initializer`, because every `OQS_ENABLE_SIG_*` family is
switched off). Upstream has a `SKIP_OQS_PROVIDER` code path for exactly this
case; the post-quantum provider is only an optional extra and nothing else
depends on it.

## Upstream build problems this repository works around

Getting the unmodified release to compile with MSVC on the current SDK needed
three fixes (all applied by `scripts/prepare-source.py`, none of them touches the
feature):

| problem | fix |
|---|---|
| the vendored `oqs-provider` fails with `error C7757` (empty initializer, every `OQS_ENABLE_SIG_*` family is off) | configure with `-DOQS_ENABLE=OFF`, which selects upstream's `SKIP_OQS_PROVIDER` path |
| `wlanapi.h` pulls `windot11.h` in and that header does not parse (`NDIS_OBJECT_HEADER` undefined) | the SSID comes from `netsh wlan show interfaces` instead |
| `BLAKE2` selection uses a generator expression, so `set_property(SOURCE <genex> ... HAVE_SSE2)` never applies and MSVC stops with "This code requires at least SSE2." | the two `set()` calls become a plain `if(HAS_SSE2)` |

## Status

The build is green: the workflow compiles the release, links every tool and
checks that both halves of the feature are in `vpncmgr.exe` (the tray entry, the
editor class and the `netsh` probe); the step fails instead of shipping half of
it. The artifact `softether-win-<version>` contains `vpncmgr.exe`,
`vpnclient.exe`, `vpnserver.exe`, `vpnbridge.exe`, `vpncmd.exe`, `vpnsetup.exe`,
`vpnsmgr.exe` and `hamcore.se2`.

What is **not** verified yet: the behaviour on a real desktop. The rule engine,
the probes and the editor window compile and link, but nobody has clicked
through them on Windows. Minimum smoke test:

1. tray icon → *Network connection rules...* opens the editor;
2. add a Wi-Fi rule, *Use current* fills the SSID, *Save* writes
   `%ProgramData%\SoftEther VPN Client\net_rules.ini`;
3. connecting that SSID connects the named connection, leaving it disconnects;
4. a wired rule for the gateway of the Ethernet adapter does the same;
5. *All connections* acts on every connection.
