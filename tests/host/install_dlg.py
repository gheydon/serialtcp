#!/usr/bin/env python3
# SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
# Copyright (C) 2026 Gordon Heydon
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
Assemble a DLG Professional installation into a host directory.

DLG ships an Amiga Installer script (Disk1:Install_DLG). Rather than click
through it in the emulator, this does the same job from the host, following
what that script and Disk1:Install/start.2 actually do. The advantage is that
it is repeatable and diffable -- and it lets the four port definitions be
generated exactly rather than typed into a config editor four times.

Port and modem definitions are fixed-size binary records (struct Port, 146
bytes; struct Modem, 138 bytes) whose layout is in DLG's own portconfig.h and
confirmed against the shipped TR0.port / DIRECTCONNECT.modem.
"""

import os
import shutil
import struct
import sys

DISKS = "/Users/gordon/Source/amiga/serialtcp/reference/dlg/disks"
DEST = "/Users/gordon/Amiga-test/DLG"
OS32 = "/Users/gordon/Amiga-test/OS32"

NODES = 4                      # TR0..TR3
DEVICE = "serialtcp.device"
MODEM_FILE = "SERIALTCP.modem"


def field(text, size):
    """Fixed-size NUL-padded field, as the Amiga structs use."""
    b = text.encode("latin-1")[: size - 1]
    return b + b"\0" * (size - len(b))


def make_port(device, unit, globals_file, modem_file, display_file):
    """struct Port -- 146 bytes."""
    rec = (
        field(device, 36)
        + bytes([unit & 0xFF])
        + field(globals_file, 36)
        + field(modem_file, 36)
        + field(display_file, 36)
        + b"\0"
    )
    assert len(rec) == 146, len(rec)
    return rec


def make_modem(init, hangup, reset, lock, hangup_method,
               max_baud, answer, answer_method, ring, command_mode):
    """struct Modem -- 138 bytes (137 used, padded to even)."""
    rec = (
        field(init, 80)
        + field(hangup, 10)
        + field(reset, 10)
        + bytes([lock & 0xFF])
        + bytes([hangup_method & 0xFF])
        + struct.pack(">I", max_baud)
        + field(answer, 10)
        + bytes([answer_method & 0xFF])
        + field(ring, 10)
        + field(command_mode, 10)
        + b"\0"
    )
    assert len(rec) == 138, len(rec)
    return rec


def copytree(src, dst):
    if not os.path.isdir(src):
        print(f"  ! missing: {src}")
        return 0
    os.makedirs(dst, exist_ok=True)
    n = 0
    for entry in os.listdir(src):
        s, d = os.path.join(src, entry), os.path.join(dst, entry)
        if os.path.isdir(s):
            n += copytree(s, d)
        else:
            shutil.copy2(s, d)
            n += 1
    return n


def copyfile(src, dst):
    if not os.path.exists(src):
        print(f"  ! missing: {src}")
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def main():
    if os.path.isdir(DEST):
        shutil.rmtree(DEST)

    for d in ("DLG", "DLGConfig", "MSG", "FILE", "USER",
              "Mail", "Mail/Inbound", "Mail/Outbound", "Mail/Nodelist", "Fido"):
        os.makedirs(os.path.join(DEST, d), exist_ok=True)

    print("Programs:")
    n = copytree(f"{DISKS}/d2/Disk2/DLG", f"{DEST}/DLG")
    n += copytree(f"{DISKS}/d3/Disk3/DLG", f"{DEST}/DLG")
    print(f"  DLG:        {n} files")

    print("Configuration:")
    n = 0
    for sub in ("Batch", "CharSets", "Languages", "Menu", "Misc",
                "Port", "Rexx", "Template", "Text"):
        n += copytree(f"{DISKS}/d5/Disk5/{sub}", f"{DEST}/DLGConfig/{sub}")
    print(f"  DLGConfig:  {n} files")

    print("Data:")
    print(f"  MSG:        {copytree(f'{DISKS}/d3/Disk3/MSG',  f'{DEST}/MSG')} files")
    print(f"  FILE:       {copytree(f'{DISKS}/d3/Disk3/File', f'{DEST}/FILE')} files")
    print(f"  Fido:       {copytree(f'{DISKS}/d4/Disk4/Fido', f'{DEST}/Fido')} files")

    # ---- system files into the AmigaOS install ----
    print("System files into AmigaOS:")
    d1 = f"{DISKS}/d1/Disk1"
    ok = 0
    ok += copyfile(f"{d1}/Libs/dlg.library_any",      f"{OS32}/Libs/dlg.library")
    ok += copyfile(f"{d1}/Libs/dlgrexx.library_Any",  f"{OS32}/Libs/dlgrexx.library")
    ok += copyfile(f"{d1}/Libs/TrapList.library",     f"{OS32}/Libs/TrapList.library")
    ok += copyfile(f"{d1}/L/TPT-Handler",             f"{OS32}/L/TPT-Handler")
    ok += copyfile(f"{d1}/L/Null-Handler",            f"{OS32}/L/Null-Handler")
    ok += copyfile(f"{d1}/Devs/Null-Mountlist",       f"{OS32}/Devs/Null-Mountlist")
    for c in ("Installer", "LhA", "MakeSysop"):
        ok += copyfile(f"{d1}/c/{c}", f"{OS32}/C/{c}")
    print(f"  copied {ok} files")

    # ---- mountlist: one entry per port ----
    # TL0 is the local console login and must always exist; DLG refuses to run
    # without it. TR0..TR3 are the dialup-equivalent nodes.
    # No leading comment: AmigaDOS Mount does not accept one before the first
    # entry and reports "Device 'TL0:' not found in file". The shipped
    # mountlist puts its comments after every entry, and so must this.
    ml = []
    for name in ["TL0"] + [f"TR{i}" for i in range(NODES)]:
        ml.append(
            f"{name}:  Handler = L:TPT-Handler\n"
            f"        Priority = 5\n"
            f"        StackSize = 6000\n"
            f"        GlobVec = -1\n"
            f"#\n\n"
        )
    ml.append("; Generated by install_dlg.py -- one entry per DLG port.\n")
    with open(f"{OS32}/Devs/TPTMountlist", "w") as f:
        f.write("".join(ml))
    print(f"  Devs/TPTMountlist: TL0 + TR0..TR{NODES-1}")

    # ---- port definitions ----
    port_dir = f"{DEST}/DLGConfig/Port"
    with open(f"{port_dir}/TL0.port", "wb") as f:
        f.write(make_port("console.device", 0, "LOCAL.globals",
                          "DIRECTCONNECT.modem", "DEFAULT.display"))
    for i in range(NODES):
        with open(f"{port_dir}/TR{i}.port", "wb") as f:
            f.write(make_port(DEVICE, i, "DEFAULT.globals",
                              MODEM_FILE, "DEFAULT.display"))
    print(f"  {NODES} remote ports -> {DEVICE} units 0..{NODES-1}")

    # ---- modem definition for the virtual modem ----
    #
    # HangupMethod 0 = command method (+++ then the hangup string). NOT DTR:
    # serial.device has no DTR control on the Amiga, so the DTR method cannot
    # work through a virtual device -- see docs/DLG-Pro.md.
    #
    # AnswerMethod 1 = the BBS answers the call itself with ATA, which is why
    # the daemon is configured with auto-answer 0. One of the two must answer,
    # never both.
    with open(f"{port_dir}/{MODEM_FILE}", "wb") as f:
        f.write(make_modem(
            init="AT&F&C1&D2E0Q0V1X4",
            hangup="ATH0",
            reset="ATZ",
            lock=1,
            hangup_method=0,
            max_baud=38400,
            answer="ATA",
            answer_method=1,
            ring="RING",
            command_mode="+++",
        ))
    print(f"  {MODEM_FILE}: command-mode hangup, BBS answers with ATA")

    # ---- startup script ----
    # Follows Disk1:Install/start.2, with one Mount and one ActivatePort per
    # port instead of just TR0.
    #
    # TPTCron is not optional. ActivatePort does not launch ResMan itself --
    # it calls CronEvent(ADDEVENT, "DLG:ResMan") and then polls for 100
    # seconds. With no cron daemon listening, nothing launches ResMan and the
    # boot sits on "Installing ResMan ..." until it gives up.
    startup = [
        "; DLG-Startup -- generated by install_dlg.py\n\n",
        "FailAt 4\n\n",
        "Assign DLG:       DLGDisk:DLG\n",
        "Assign DLGConfig: DLGDisk:DLGConfig\n",
        "Assign MSG:       DLGDisk:MSG\n",
        "Assign FILE:      DLGDisk:FILE\n",
        "Assign USER:      DLGDisk:USER\n",
        "Assign Mail:      DLGDisk:Mail\n",
        "Assign Inbound:   DLGDisk:Mail/Inbound\n",
        "Assign Outbound:  DLGDisk:Mail/Outbound\n",
        "Assign Nodelist:  DLGDisk:Mail/Nodelist\n",
        "Assign Fido:      DLGDisk:Fido\n\n",
        "Path DLG: add\n",
        "Stack 25000\n\n",
        "; TPTCron logs to NULL:, so that has to exist first.\n",
        "Mount NULL: from Devs:Null-Mountlist\n\n",
        "Run >NIL: DLG:TPTCron -t DLGConfig:Batch/CronTab_NoFido -b NULL:\n",
        "Wait 2\n\n",
        "FailAt 10\n\n",
        "Mount TL0: from Devs:TPTMountlist\n",
    ]
    for i in range(NODES):
        startup.append(f"Mount TR{i}: from Devs:TPTMountlist\n")

    startup.append('\nDLG:CronEvent >NIL: add 0 "dlg:tptbc"\n')
    startup.append("Wait 2\n\n")
    startup.append("; TL0 must always be activated or DLG will not run.\n")
    startup.append('DLG:ActivatePort -p TL0 -b ""\n')
    for i in range(NODES):
        startup.append(f"DLG:ActivatePort -p TR{i}\n")
    startup.append('\nEcho "DLG: TL0 + TR0..TR%d activated"\n' % (NODES - 1))

    with open(f"{OS32}/S/DLG-Startup", "w") as f:
        f.write("".join(startup))
    print(f"  S/DLG-Startup: mounts and activates TL0 + TR0..TR{NODES-1}")

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
