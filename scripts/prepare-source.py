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


def fix_blake2_sse2() -> None:
    """Pick the BLAKE2 implementation with a plain if/else.

    Upstream selects the source through a generator expression and then sets a
    source property on it, which CMake cannot do for expressions: the HAVE_SSE2
    definition never reaches the compiler and MSVC fails with
    "This code requires at least SSE2.".
    """
    log("fixing the BLAKE2 SSE2 selection")
    cm = SOURCE / "src" / "Cedar" / "CMakeLists.txt"
    data = cm.read_bytes().decode("utf-8", "surrogateescape")

    old = (
        "  set(BLAKE2_SRC_PATH $<IF:$<BOOL:${HAS_SSE2}>,${TOP_DIRECTORY}/3rdparty/BLAKE2/sse,"
        "${TOP_DIRECTORY}/3rdparty/BLAKE2/ref>)\n"
        "  set(BLAKE2_SRC $<IF:$<BOOL:${HAS_SSE2}>,${BLAKE2_SRC_PATH}/blake2s.c,"
        "${BLAKE2_SRC_PATH}/blake2s-ref.c>)\n"
    )
    data = data.replace("\r\n", "\n")
    if data.count(old) != 1:
        sys.exit("BLAKE2 selection not found, upstream changed")

    new = (
        "  if(HAS_SSE2)\n"
        "    set(BLAKE2_SRC_PATH \"${TOP_DIRECTORY}/3rdparty/BLAKE2/sse\")\n"
        "    set(BLAKE2_SRC \"${BLAKE2_SRC_PATH}/blake2s.c\")\n"
        "  else()\n"
        "    set(BLAKE2_SRC_PATH \"${TOP_DIRECTORY}/3rdparty/BLAKE2/ref\")\n"
        "    set(BLAKE2_SRC \"${BLAKE2_SRC_PATH}/blake2s-ref.c\")\n"
        "  endif()\n"
    )
    data = data.replace(old, new)
    cm.write_bytes(data.replace("\n", "\r\n").encode("utf-8", "surrogateescape"))
    log("  blake2 source is now a concrete path, HAVE_SSE2 reaches the compiler")


def patch_sfx_dialog() -> None:
    """Extract at once instead of waiting for the first timer tick.

    Windows only synthesises WM_TIMER while the message queue is empty.  The
    self extracting dialog of our own build repaints without ever settling on
    a headless build server, so the 500 ms timer never fires and the installer
    sits in front of its "Preparing to install..." dialog forever.  Posting
    the timer message ourselves runs the extraction before any paint message
    is dispatched.
    """
    log("patching the self extracting dialog")
    sw = SOURCE / "src" / "Cedar" / "SW.c"
    inject(
        sw,
        "\tcase WM_INITDIALOG:\r\n\t\tSetTimer(hWnd, 1, 500, NULL);\r\n",
        "\r\n\t\t// The dialog repaint can starve WM_TIMER, so extract now\r\n"
        "\t\tPostMessage(hWnd, WM_TIMER, (WPARAM)1, 0);\r\n",
    )
    inject(
        sw,
        "// SFX extraction dialog procedure\r\n"
        "bool CALLBACK SfxModeMainDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)\r\n"
        "{\r\n\tUINT ret;\r\n\tbool hide_msg = false;\r\n",
        "\tSfxTraceMessage(msg, wParam);\r\n",
    )
    inject(
        sw,
        "// SFX extraction dialog procedure\r\n",
        "// Writes the dialog messages to the file named by SOFTETHER_SFX_TRACE\r\n"
        "static void SfxTraceMessage(UINT msg, WPARAM wParam)\r\n"
        "{\r\n"
        "\twchar_t path[MAX_PATH];\r\n"
        "\tHANDLE h;\r\n"
        "\tchar line[64];\r\n"
        "\tint len;\r\n"
        "\r\n"
        "\tif (GetEnvironmentVariableW(L\"SOFTETHER_SFX_TRACE\", path, MAX_PATH) == 0)\r\n"
        "\t{\r\n\t\treturn;\r\n\t}\r\n"
        "\r\n"
        "\th = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,\r\n"
        "\t\tOPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);\r\n"
        "\tif (h == INVALID_HANDLE_VALUE)\r\n\t{\r\n\t\treturn;\r\n\t}\r\n"
        "\tlen = sprintf_s(line, sizeof(line), \"%u %u\\r\\n\", (UINT)msg, (UINT)wParam);\r\n"
        "\tif (len > 0)\r\n\t{\r\n\t\tWriteFile(h, line, (DWORD)len, NULL, NULL);\r\n\t}\r\n"
        "\tCloseHandle(h);\r\n"
        "}\r\n"
        "\r\n",
    )


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

    patch_sfx_dialog()

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

    fix_blake2_sse2()
    install_feature()
    log("done")


if __name__ == "__main__":
    main()
