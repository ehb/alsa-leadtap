#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Package the LibreELEC add-on service.alsa-leadtap as an installable zip.

The add-on holds the aarch64 plugin, the aarch64 wled-audiosync binary, a start script, a
systemd unit and the settings. It is tied to the architecture, not to a kernel or LibreELEC
version. Version: --version, else the newest entry of CHANGELOG.md.

  python libreelec/build_addon.py --so build/aarch64/libasound_module_pcm_leadtap.so \\
      --audiosync ../wled-audiosync/build/aarch64/wled-audiosync \\
      --audiosync-license path/to/LICENSE [--version 0.1.0] [--out build]

Both binaries are checked for ELF class 64-bit, little endian, machine AArch64.
"""
import argparse
import os
import re
import struct
import sys
import zipfile

ID = "service.alsa-leadtap"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADDON = os.path.join(HERE, "addon")
EM_AARCH64 = 183


def elf_problem(path):
    """None if path is a little-endian 64-bit AArch64 ELF, else the reason."""
    with open(path, "rb") as f:
        head = f.read(20)
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return "not an ELF file"
    if head[4] != 2 or head[5] != 1:
        return "not a 64-bit little-endian ELF"
    machine = struct.unpack_from("<H", head, 18)[0]
    if machine != EM_AARCH64:
        return "machine %d, not AArch64 (%d)" % (machine, EM_AARCH64)
    return None


def changelog_version():
    text = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="ascii").read()
    m = re.search(r"^## (\d+\.\d+\.\d+)\s*$", text, re.M)
    if not m:
        sys.exit("CHANGELOG.md has no '## x.y.z' entry; pass --version")
    return m.group(1)


def read(*parts):
    with open(os.path.join(*parts), "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", required=True, help="aarch64 libasound_module_pcm_leadtap.so")
    ap.add_argument("--audiosync", required=True, help="aarch64 wled-audiosync binary")
    ap.add_argument("--audiosync-license", required=True,
                    help="license text of wled-audiosync (GPL-3.0), shipped next to the binary")
    ap.add_argument("--version", help="add-on version x.y.z (default: CHANGELOG.md)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build"))
    a = ap.parse_args()

    for label, path in (("--so", a.so), ("--audiosync", a.audiosync)):
        if not os.path.isfile(path):
            sys.exit("%s: no such file: %s" % (label, path))
        why = elf_problem(path)
        if why:
            sys.exit("%s %s: %s -- this add-on is for 64-bit Raspberry Pi (aarch64) only" % (label, path, why))
    if not os.path.isfile(a.audiosync_license):
        sys.exit("--audiosync-license: no such file: %s" % a.audiosync_license)

    version = a.version or changelog_version()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit("version %r is not x.y.z" % version)
    xml = open(os.path.join(ADDON, "addon.xml.in"), encoding="ascii").read()
    xml = xml.replace("@VERSION@", version)

    files = [  # (path in the add-on, bytes, executable)
        ("addon.xml", xml.encode("ascii"), False),
        ("default.py", read(ADDON, "default.py"), False),
        ("LICENSE.txt", read(ROOT, "LICENSE"), False),
        ("LICENSE-wled-audiosync.txt", read(a.audiosync_license), False),
        ("bin/alsa-leadtap", read(ADDON, "bin", "alsa-leadtap"), True),
        ("bin/wled-audiosync", read(a.audiosync), True),
        ("lib/libasound_module_pcm_leadtap.so", read(a.so), False),
        ("system.d/%s.service" % ID, read(ADDON, "system.d", ID + ".service"), False),
        ("resources/settings.xml", read(ADDON, "resources", "settings.xml"), False),
        ("resources/language/resource.language.en_gb/strings.po",
         read(ADDON, "resources", "language", "resource.language.en_gb", "strings.po"), False),
    ]
    for name, data, _ in files:
        if name.endswith((".service", ".py", ".xml", ".po")) or name == "bin/alsa-leadtap":
            if b"\r" in data:
                sys.exit("%s has CR line endings" % name)

    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, "%s-%s.zip" % (ID, version))
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data, exe in files:
            zi = zipfile.ZipInfo("%s/%s" % (ID, name), date_time=(2026, 1, 1, 0, 0, 0))
            zi.external_attr = ((0o755 if exe else 0o644) | 0o100000) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
    print(zpath)


if __name__ == "__main__":
    main()
