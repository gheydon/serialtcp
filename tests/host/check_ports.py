#!/usr/bin/env python3
# SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.

"""
Audit a DLG Professional multi-node setup.

Every port needs more than its .port record, and DLG says nothing when a piece
is missing -- a node will mount, activate, serve exactly one caller and then
stop for good. This reads each port definition, follows what it points at, and
reports anything absent.

    check_ports.py [system-root]        default /Users/gordon/Amiga-test/hdf-stage
"""

import os
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "/Users/gordon/Amiga-test/hdf-stage"
CFG = os.path.join(ROOT, "DLGConfig")
PORTDIR = os.path.join(CFG, "Port")
BATCHDIR = os.path.join(CFG, "Batch")


def find(directory, name):
    """Case-insensitive lookup, because AmigaDOS is case-insensitive."""
    if not os.path.isdir(directory):
        return None
    low = name.lower()
    for entry in os.listdir(directory):
        if entry.lower() == low:
            return os.path.join(directory, entry)
    return None


def read_port(path):
    """struct Port: Device[36], Unit, GlobalFile[36], ModemFile[36],
    DisplayFile[36], pad -- 146 bytes."""
    d = open(path, "rb").read()
    if len(d) != 146:
        return None

    def s(off):
        raw = d[off:off + 36]
        return raw.split(b"\0")[0].decode("latin-1")

    return {
        "device": s(0),
        "unit": d[36],
        "globals": s(37),
        "modem": s(73),
        "display": s(109),
    }


def main():
    if not os.path.isdir(PORTDIR):
        print(f"no port directory at {PORTDIR}")
        return 2

    ports = sorted(f[:-5] for f in os.listdir(PORTDIR) if f.lower().endswith(".port"))
    if not ports:
        print("no .port files found")
        return 2

    mountlist = ""
    ml = os.path.join(ROOT, "Devs", "TPTMountlist")
    if os.path.exists(ml):
        mountlist = open(ml, errors="replace").read().lower()

    startup = ""
    ss = os.path.join(ROOT, "S", "DLG-Startup")
    if os.path.exists(ss):
        startup = open(ss, errors="replace").read().lower()

    problems = 0
    print(f"{'port':6} {'device':18} {'unit':>4}  checks")
    print("-" * 78)

    for p in ports:
        cfg = read_port(os.path.join(PORTDIR, p + ".port"))
        if not cfg:
            print(f"{p:6} {'(unreadable .port)':18}")
            problems += 1
            continue

        issues = []

        # The session batch. Its FreePort call is what returns the port to
        # ResMan; without it the node serves one call and stops.
        batch = find(BATCHDIR, p + ".startup")
        if not batch:
            issues.append(f"MISSING DLGConfig:Batch/{p}.Startup")
        else:
            body = open(batch, errors="replace").read().lower()
            if "freeport" not in body:
                issues.append(f"{os.path.basename(batch)} has no FreePort")
            elif f"-p {p.lower()}" not in body:
                issues.append(f"{os.path.basename(batch)} FreePort names the wrong port")

        for label, name, folder in (
            ("globals", cfg["globals"], PORTDIR),
            ("modem",   cfg["modem"],   PORTDIR),
            ("display", cfg["display"], PORTDIR),
        ):
            if not name:
                issues.append(f"no {label} file set")
            elif not find(folder, name):
                issues.append(f"MISSING {label} '{name}'")

        if mountlist and f"{p.lower()}:" not in mountlist:
            issues.append("no TPTMountlist entry")
        if startup:
            if f"mount {p.lower()}:" not in startup:
                issues.append("not mounted in DLG-Startup")
            if f"activateport -p {p.lower()}" not in startup:
                issues.append("not activated in DLG-Startup")

        status = "ok" if not issues else "; ".join(issues)
        print(f"{p:6} {cfg['device']:18} {cfg['unit']:>4}  {status}")
        problems += len(issues)

    print("-" * 78)
    print("all ports correctly configured" if not problems
          else f"{problems} problem(s) found")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
