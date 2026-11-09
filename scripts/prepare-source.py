#!/usr/bin/env python3
"""Fetch the upstream SoftEther VPN source, patch it and inject the netrule feature.

Everything is done with assertions: if upstream changes the anchors the script
fails loudly instead of producing a half patched tree.

Windows runners have no `patch(1)`, so the unified diffs are applied here.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "source"
VERSION = os.environ.get("SOFTETHER_VERSION", "5.2.5189")
TARBALL = f"SoftEtherVPN-{VERSION}.tar.xz"
URL = f"https://github.com/softethervpn/softethervpn/releases/download/{VERSION}/{TARBALL}"


def log(msg: str) -> None:
    print(f"[prepare] {msg}", flush=True)


# --------------------------------------------------------------------- source
def fetch() -> None:
    if SOURCE.is_dir():
        log("source/ already present")
        return

    archive = ROOT / TARBALL
    if not archive.is_file():
        log(f"downloading {URL}")
        urllib.request.urlretrieve(URL, archive)

    log(f"extracting {archive.name}")
    tmp = ROOT / "_extract"
    if tmp.is_dir():
        shutil.rmtree(tmp)
    tmp.mkdir()
    with tarfile.open(archive) as tf:
        tf.extractall(tmp)
    inner = next(p for p in tmp.iterdir() if p.is_dir())
    inner.rename(SOURCE)
    shutil.rmtree(tmp)


# ---------------------------------------------------------------------- patch
def apply_patch(path: Path) -> None:
    """Apply a unified diff to a CRLF tree, keeping the line endings untouched."""
    log(f"applying {path.name}")
    raw = path.read_bytes().decode("utf-8", "surrogateescape")
    lines = raw.split("\n")

    target = None
    target_lines: list[str] = []
    pos = 0

    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("--- "):
            old = line[4:].split("\t")[0].strip()
            new = lines[i + 1][4:].split("\t")[0].strip()
            name = new if new != "/dev/null" else old
            name = re.sub(r"^[ab]/", "", name)
            target = SOURCE / name
            if not target.is_file():
                sys.exit(f"patch target missing: {target}")
            target_lines = target.read_bytes().decode("utf-8", "surrogateescape").split("\n")
            pos = 0
            i += 2
            continue

        if line.startswith("@@"):
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            if not m:
                sys.exit(f"bad hunk header: {line}")
            want = int(m.group(1)) - 1
            i += 1
            while i < len(lines) and not lines[i].startswith(("@@", "--- ")):
                hl = lines[i]
                if hl.startswith("+") or hl.startswith("-") or hl.startswith(" ") or hl == "":
                    body = hl[1:] if hl[:1] in "+- " else ""
                    if hl.startswith("+"):
                        target_lines.insert(want, body)
                        want += 1
                    elif hl.startswith("-"):
                        actual = target_lines[want]
                        if actual.rstrip("\r") != body.rstrip("\r"):
                            sys.exit(f"{target.name}: expected {body!r}, found {actual!r}")
                        del target_lines[want]
                    elif hl.startswith(" "):
                        actual = target_lines[want]
                        if actual.rstrip("\r") != body.rstrip("\r"):
                            sys.exit(f"{target.name}: context mismatch at line {want + 1}")
                        want += 1
                i += 1
            continue

        i += 1

    if target is not None:
        target.write_bytes("\n".join(target_lines).encode("utf-8", "surrogateescape"))
        log(f"  patched {target.relative_to(SOURCE)}")


# ------------------------------------------------------------------- feature
def inject(path: Path, anchor: str, addition: str, count: int = 1) -> None:
    data = path.read_bytes().decode("utf-8", "surrogateescape")
    found = data.count(anchor)
    if found != count:
        sys.exit(f"{path.name}: anchor found {found} times, expected {count}: {anchor[:60]!r}")
    data = data.replace(anchor, anchor + addition, count)
    path.write_bytes(data.encode("utf-8", "surrogateescape"))
    log(f"  hooked {path.relative_to(SOURCE)}")


def install_feature() -> None:
    log("installing the netrule feature")
    for name in ("netrule.c", "netrule.h"):
        shutil.copy2(ROOT / "feature" / name, SOURCE / "src" / "Cedar" / name)

    cm = SOURCE / "src" / "Cedar" / "CM.c"

    # 1) the prototypes
    inject(cm, "#define MICROSOFT_C\r\n", '\r\n#include "netrule.h"\r\n')

    # 2) a silent mode for the connect / disconnect helpers the monitor calls
    inject(
        cm,
        "bool CmWarningDesktop(HWND hWnd, wchar_t *account_name)\r\n{\r\n"
        "\t// Validate arguments\r\n\tif (hWnd == NULL || account_name == NULL)\r\n\t{\r\n"
        "\t\treturn false;\r\n\t}\r\n",
        "\r\n\t// The network rule monitor acts without the user\r\n\tif (nr_auto_mode)\r\n\t{\r\n"
        "\t\treturn true;\r\n\t}\r\n",
    )

    # 3) the entry in the tray menu
    inject(
        cm,
        '\tMsAppendMenu(h, MF_ENABLED | MF_STRING, 100007, _UU("CM_TRAY_MENU_CANCEL"));\r\n',
        '\r\n\t// Network connection rules\r\n'
        '\tMsAppendMenu(h, MF_SEPARATOR, 10008, NULL);\r\n'
        "\tMsAppendMenu(h, MF_ENABLED | MF_STRING, NR_MENU_ID, L\"Network connection rules...\");\r\n",
    )

    # 4) dispatch that command
    inject(
        cm,
        "\twchar_t *selected_name = NULL;\r\n\tUINT starter_id = 0;\r\n"
        "\tif (hWnd == NULL)\r\n\t{\r\n\t\treturn;\r\n\t}\r\n",
        "\r\n\tif (LOWORD(wParam) == NR_MENU_ID)\r\n\t{\r\n\t\tNrRulesDialog(hWnd);\r\n\t\treturn;\r\n\t}\r\n",
    )

    # 5) start and stop the monitor around the message loop
    inject(cm, "\t// Initialize\r\n\tInitCM(true);\r\n", "\r\n\t// Network rule monitor\r\n\tNrMonitorStart();\r\n")
    inject(cm, "\t// Release\r\n\tFreeCM();\r\n", "\r\n\tNrMonitorStop();\r\n", count=1)

    # 6) the WLAN and IP helper libraries
    inject(
        SOURCE / "src" / "Cedar" / "CMakeLists.txt",
        "target_link_libraries(cedar PUBLIC mayaqua)",
        "\n\nif(WIN32)\n  # used by the network rule monitor (route table)\n"
        "  target_link_libraries(cedar PUBLIC iphlpapi)\nendif()",
    )


def main() -> None:
    fetch()

    patches = sorted((ROOT / "patches").glob("*.patch"))
    prepared = (SOURCE / "src" / "Cedar" / "netrule.c").is_file()

    if prepared:
        # a local re-run: refresh the feature files, the hooks are already there
        log("source/ is already prepared, only refreshing the feature files")
        for name in ("netrule.c", "netrule.h"):
            shutil.copy2(ROOT / "feature" / name, SOURCE / "src" / "Cedar" / name)
        return

    for patch in patches:
        apply_patch(patch)

    # the is_restricted patch must have taken effect
    server_c = (SOURCE / "src" / "Cedar" / "Server.c").read_text(encoding="utf-8", errors="surrogateescape")
    if re.search(r"^\s*is_restricted = SiIsEnterpriseFunctionsRestrictedOnOpenSource", server_c, re.M):
        sys.exit("is_restricted is still assigned")

    install_feature()
    log("done")


if __name__ == "__main__":
    main()
