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

## Status

The native part builds in CI. The rule engine and the editor window compile but
have not been exercised on a real Windows desktop yet: the WLAN probe, the wired
gateway probe and the connect/disconnect path need a manual smoke test before
this can be considered production ready.
