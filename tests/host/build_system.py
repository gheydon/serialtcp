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
Build a bootable AmigaOS 3.2 hard drive with DLG Professional installed.

A real install: the OS files come from the Hyperion install floppies on the 3.2
CD, merged the way the installer merges them, so LIBS: is complete (diskfont
included -- its absence is what silently stopped DLG last time). DLG comes from
its own five install disks, laid out as Disk1:Install_DLG lays it out.

Everything is assembled on the host and written into the image in one pass,
which keeps it repeatable and inspectable. The emulator's mouse is not
absolute, so driving two installer GUIs by screenshot would be slow and
fragile for no benefit.

The result is DH0: -- a genuine hard drive, not the CD's Preinstallation
Environment. SerialTCP itself stays on a separate host-directory drive so
binaries and logs can be changed and read without rebuilding the image.
"""

import os
import shutil
import struct
import subprocess
import sys

BASE    = "/Users/gordon/Amiga-test"
ADF     = f"{BASE}/OS32/ADF"
UNPACK  = f"{BASE}/adf-unpacked"
STAGE   = f"{BASE}/hdf-stage"
XDFTOOL = f"{BASE}/.venv/bin/xdftool"
IMAGE   = f"{BASE}/Workbench.hdf"
DISKS   = "/Users/gordon/Source/amiga/serialtcp/reference/dlg/disks"

SIZE       = "600Mi"
NODES      = 4
DEVICE     = "serialtcp.device"
MODEM_FILE = "SERIALTCP.modem"

OS_LAYOUT = [
    ("Workbench3.2",     ""),
    ("Extras3.2",        ""),
    ("Classes3.2",       ""),
    ("ModulesA1200_3.2", ""),
    ("Fonts",            "Fonts"),
    ("Locale",           "Locale"),
    ("Storage3.2",       "Storage"),
    ("Install3.2",       "Install"),
]

SKIP = {"Disk.info", "SYStoInstallDisk", "SYStoInstallDisk.info",
        "ReadMe.txt", "ReadMe.txt.info"}


# ---------------------------------------------------------------- helpers

def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ! {' '.join(args[1:3])}: {(r.stderr or r.stdout).strip()[:160]}")
    return r.returncode == 0


def merge(src, dst):
    n = 0
    os.makedirs(dst, exist_ok=True)
    for entry in os.listdir(src):
        if entry in SKIP:
            continue
        s, d = os.path.join(src, entry), os.path.join(dst, entry)
        if os.path.isdir(s):
            n += merge(s, d)
        else:
            shutil.copy2(s, d)
            n += 1
    return n


def field(text, size):
    b = text.encode("latin-1")[: size - 1]
    return b + b"\0" * (size - len(b))


def make_port(device, unit, globals_file, modem_file, display_file):
    rec = (field(device, 36) + bytes([unit & 0xFF]) + field(globals_file, 36)
           + field(modem_file, 36) + field(display_file, 36) + b"\0")
    assert len(rec) == 146
    return rec


def make_modem(init, hangup, reset, lock, hangup_method, max_baud,
               answer, answer_method, ring, command_mode):
    rec = (field(init, 80) + field(hangup, 10) + field(reset, 10)
           + bytes([lock & 0xFF]) + bytes([hangup_method & 0xFF])
           + struct.pack(">I", max_baud) + field(answer, 10)
           + bytes([answer_method & 0xFF]) + field(ring, 10)
           + field(command_mode, 10) + b"\0")
    assert len(rec) == 138
    return rec


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


# ---------------------------------------------------------------- stages

def unpack_os():
    if os.path.isdir(UNPACK):
        shutil.rmtree(UNPACK)
    os.makedirs(UNPACK)
    for disk, _ in OS_LAYOUT:
        src = f"{ADF}/{disk}.adf"
        if os.path.exists(src):
            run([XDFTOOL, src, "unpack", f"{UNPACK}/{disk}"])
    print(f"  unpacked {len(OS_LAYOUT)} install floppies")


def stage_os():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    total = 0
    for disk, target in OS_LAYOUT:
        src = f"{UNPACK}/{disk}"
        if os.path.isdir(src):
            total += merge(src, os.path.join(STAGE, target) if target else STAGE)
    print(f"  AmigaOS 3.2: {total} files")
    return total


def stage_dlg():
    d1 = f"{DISKS}/d1/Disk1"
    total = 0

    for sub in ("DLG", "DLGConfig", "MSG", "FILE", "USER",
                "Mail", "Mail/Inbound", "Mail/Outbound", "Mail/Nodelist", "Fido"):
        os.makedirs(f"{STAGE}/{sub}", exist_ok=True)

    total += merge(f"{DISKS}/d2/Disk2/DLG",  f"{STAGE}/DLG")
    total += merge(f"{DISKS}/d3/Disk3/DLG",  f"{STAGE}/DLG")
    for sub in ("Batch", "CharSets", "Languages", "Menu", "Misc",
                "Port", "Rexx", "Template", "Text"):
        total += merge(f"{DISKS}/d5/Disk5/{sub}", f"{STAGE}/DLGConfig/{sub}")
    total += merge(f"{DISKS}/d3/Disk3/MSG",  f"{STAGE}/MSG")
    total += merge(f"{DISKS}/d3/Disk3/File", f"{STAGE}/FILE")
    total += merge(f"{DISKS}/d4/Disk4/Fido", f"{STAGE}/Fido")

    # DLG's own system files, into the real LIBS:, L: and DEVS:
    for src, dst in [
        ("Libs/dlg.library_any",     "Libs/dlg.library"),
        ("Libs/dlgrexx.library_Any", "Libs/dlgrexx.library"),
        ("Libs/TrapList.library",    "Libs/TrapList.library"),
        ("L/TPT-Handler",            "L/TPT-Handler"),
        ("L/Null-Handler",           "L/Null-Handler"),
        ("Devs/Null-Mountlist",      "Devs/Null-Mountlist"),
        ("c/Installer",              "C/Installer"),
        ("c/LhA",                    "C/LhA"),
        ("c/MakeSysop",              "C/MakeSysop"),
    ]:
        s, d = f"{d1}/{src}", f"{STAGE}/{dst}"
        if os.path.exists(s):
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)
            total += 1

    print(f"  DLG Professional: {total} files")
    return total


def stage_config():
    # Mountlist. No leading comment: AmigaDOS Mount rejects one before the
    # first entry, reporting "Device 'TL0:' not found in file".
    ml = []
    for name in ["TL0"] + [f"TR{i}" for i in range(NODES)]:
        ml.append(f"{name}:  Handler = L:TPT-Handler\n"
                  f"        Priority = 5\n"
                  f"        StackSize = 6000\n"
                  f"        GlobVec = -1\n#\n\n")
    ml.append("; Generated by build_system.py -- one entry per DLG port.\n")
    write(f"{STAGE}/Devs/TPTMountlist", "".join(ml))

    port_dir = f"{STAGE}/DLGConfig/Port"
    os.makedirs(port_dir, exist_ok=True)
    with open(f"{port_dir}/TL0.port", "wb") as f:
        f.write(make_port("console.device", 0, "LOCAL.globals",
                          "DIRECTCONNECT.modem", "DEFAULT.display"))
    for i in range(NODES):
        with open(f"{port_dir}/TR{i}.port", "wb") as f:
            f.write(make_port(DEVICE, i, "DEFAULT.globals",
                              MODEM_FILE, "DEFAULT.display"))

    # HangupMethod 0 = command method (+++ then ATH0). Not DTR: serial.device
    # has no DTR control, so the DTR method cannot work through a virtual
    # device. AnswerMethod is left at 1 but the modem is configured to
    # auto-answer, in which case the DLG manual says the Answer String is
    # ignored -- exactly one side must pick up the line.
    with open(f"{port_dir}/{MODEM_FILE}", "wb") as f:
        f.write(make_modem("AT&F&C1&D2E0Q0V1X4", "ATH0", "ATZ", 1, 0,
                           38400, "ATA", 1, "RING", "+++"))

    # Per-port session batch. The distribution ships TR0.Startup and
    # TL0.startup only, because it is configured for a single remote node --
    # and the last thing that script does is
    #
    #     DLG:FreePort -p <port> -k "BBS"
    #
    # which hands the port back to ResMan so it can start the next SetUp.
    # Without one, a node serves exactly one call and then stops for good:
    # ResMan still believes the port is active and never restarts it.
    template = None
    tmpl_path = f"{STAGE}/DLGConfig/Batch/TR0.Startup"
    if os.path.exists(tmpl_path):
        template = open(tmpl_path, "r", errors="replace").read()

    if template:
        for i in range(1, NODES):
            body = template.replace("tr0", f"tr{i}").replace("TR0", f"TR{i}")
            write(f"{STAGE}/DLGConfig/Batch/TR{i}.Startup", body)
        print(f"  per-port session batches: TR1..TR{NODES-1} from TR0.Startup")

    dlg_startup = ["; DLG-Startup -- generated by build_system.py\n\n",
                   "FailAt 4\n\n",
                   "Assign DLG:       SYS:DLG\n",
                   "Assign DLGConfig: SYS:DLGConfig\n",
                   "Assign MSG:       SYS:MSG\n",
                   "Assign FILE:      SYS:FILE\n",
                   "Assign USER:      SYS:USER\n",
                   "Assign Mail:      SYS:Mail\n",
                   "Assign Inbound:   SYS:Mail/Inbound\n",
                   "Assign Outbound:  SYS:Mail/Outbound\n",
                   "Assign Nodelist:  SYS:Mail/Nodelist\n",
                   "Assign Fido:      SYS:Fido\n\n",
                   "Path DLG: add\n",
                   "Stack 25000\n\n",
                   "; TPTCron logs to NULL:, and ActivatePort will not bring up\n",
                   "; ResMan without a cron daemon listening for it.\n",
                   "Mount NULL: from Devs:Null-Mountlist\n\n",
                   "Run >NIL: DLG:TPTCron -t DLGConfig:Batch/CronTab_NoFido -b NULL:\n",
                   "Wait 2\n\nFailAt 10\n\n",
                   "Mount TL0: from Devs:TPTMountlist\n"]
    for i in range(NODES):
        dlg_startup.append(f"Mount TR{i}: from Devs:TPTMountlist\n")
    dlg_startup.append('\nDLG:CronEvent >NIL: add 0 "dlg:tptbc"\nWait 2\n\n')
    dlg_startup.append("; TL0 must always be activated or DLG will not run.\n")
    dlg_startup.append('DLG:ActivatePort -p TL0 -b ""\n')
    for i in range(NODES):
        dlg_startup.append(f"DLG:ActivatePort -p TR{i}\n")
    write(f"{STAGE}/S/DLG-Startup", "".join(dlg_startup))

    # The install floppies carry the KickDisk Startup-Sequence, which never
    # runs S:User-Startup and reassigns SYS: back to the install disk at the
    # end. A hard drive needs the HD variant: the full set of assigns, and the
    # User-Startup call that everything else hangs off.
    write(f"{STAGE}/S/Startup-Sequence",
          "; Startup-Sequence for a hard drive install of AmigaOS 3.2\n\n"
          "Version exec.library version 47 >NIL:\n"
          "If Warn\n"
          "  Echo \" Forcing new ROM modules. A reboot will happen next.\"\n"
          "  LoadModule L:System-Startup ROMUPDATE DOWNGRADE\n"
          "EndIf\n\n"
          "C:SetPatch QUIET\n"
          "C:Version >NIL:\n"
          "FailAt 21\n\n"
          "C:MakeDir RAM:T RAM:ENV RAM:ENV/Sys RAM:Clipboards\n"
          "C:Copy >NIL: ENVARC: RAM:ENV ALL QUIET NOREQ\n\n"
          "Assign >NIL: ENV:      RAM:ENV\n"
          "Assign >NIL: T:        RAM:T\n"
          "Assign >NIL: CLIPS:    RAM:Clipboards\n"
          "Assign >NIL: REXX:     S:\n"
          "Assign >NIL: PRINTERS: DEVS:Printers\n"
          "Assign >NIL: KEYMAPS:  DEVS:Keymaps\n"
          "Assign >NIL: LOCALE:   SYS:Locale\n"
          "Assign >NIL: LIBS:     SYS:Classes ADD\n"
          "Assign >NIL: HELP:     LOCALE:Help DEFER\n\n"
          "BindDrivers\n\n"
          "SetEnv Language \"english\"\n"
          "SetEnv Workbench $Workbench\n"
          "SetEnv Kickstart $Kickstart\n"
          "UnSet Workbench\n"
          "UnSet Kickstart\n\n"
          "Path >NIL: RAM: C: SYS:Utilities SYS:Rexxc SYS:System S: "
          "SYS:Prefs SYS:WBStartup SYS:Tools\n\n"
          "If EXISTS S:User-Startup\n"
          "   Execute S:User-Startup\n"
          "EndIf\n\n"
          "LoadWB\n"
          "EndCLI >NIL:\n")

    # User-Startup: the daemon must be up before DLG opens any unit.
    # Nothing here keeps the startup shell: SerialTCPd puts itself into the
    # background, and DLG's startup is launched with Run, so the boot carries
    # straight on to LoadWB.
    write(f"{STAGE}/S/User-Startup",
          "; --- MUI ---------------------------------------------------------\n"
          "Assign >NIL: MUI:  SYS:MUI\n"
          "Assign >NIL: LIBS: MUI:Libs ADD\n"
          "; -----------------------------------------------------------------\n\n"
          "; --- SerialTCP + DLG ---------------------------------------------\n"
          "; Work: is a separate drive holding the SerialTCP build, its config\n"
          "; and its logs, so they can be changed without touching the system.\n"
          "Assign >NIL: DEVS: Work: ADD\n\n"
          "; No Run: the daemon detaches itself and hands the shell back.\n"
          "Work:SerialTCPd Work:serialtcp.conf\n"
          "Wait 3\n\n"
          "Run >NIL: <NIL: Execute Work:StatusLoop\n\n"
          "; Mounting and activating five ports takes the better part of a\n"
          "; minute, so DLG comes up in the background as well.  Its output\n"
          "; goes to NIL:, not to a file on Work: -- pointed at the host\n"
          "; directory drive, DLG-Startup stalled before activating a port.\n"
          "Run >NIL: <NIL: Execute S:DLG-Startup\n\n"
          "; The MUI status window, on the Workbench screen.\n"
          "Wait 3\n"
          "Run >NIL: <NIL: Work:SerialTCPStat\n"
          "; -----------------------------------------------------------------\n")

    print(f"  config: {NODES} ports on {DEVICE} units 0..{NODES-1}, "
          f"mountlist, DLG-Startup, User-Startup")


def build_image():
    if os.path.exists(IMAGE):
        os.remove(IMAGE)
    if not run([XDFTOOL, IMAGE, "create", f"size={SIZE}"]):
        return False
    if not run([XDFTOOL, IMAGE, "format", "Workbench", "ffs+intl"]):
        return False

    entries = sorted(os.listdir(STAGE))
    for e in entries:
        run([XDFTOOL, IMAGE, "write", os.path.join(STAGE, e)])
    print(f"  wrote {len(entries)} top-level entries into {os.path.basename(IMAGE)}")
    return True


def main():
    print("Unpacking AmigaOS 3.2 install floppies:")
    unpack_os()
    print("\nStaging a real system:")
    stage_os()
    stage_dlg()
    stage_config()

    print("\nChecks:")
    for p in ("Libs/diskfont.library", "S/Startup-Sequence", "S/User-Startup",
              "S/DLG-Startup", "Devs/TPTMountlist", "DLG/ActivatePort",
              "DLGConfig/Port/TR3.port", "Libs/dlg.library"):
        print(f"  {'ok ' if os.path.exists(f'{STAGE}/{p}') else 'MISSING'} {p}")

    print("\nPort audit:")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import check_ports
        check_ports.ROOT = STAGE
        check_ports.CFG = os.path.join(STAGE, "DLGConfig")
        check_ports.PORTDIR = os.path.join(check_ports.CFG, "Port")
        check_ports.BATCHDIR = os.path.join(check_ports.CFG, "Batch")
        if check_ports.main() != 0:
            print("  refusing to build an image with mis-configured ports")
            return 1
    except ImportError:
        print("  (check_ports.py not found, skipping)")

    print("\nBuilding the hard drive image:")
    if not build_image():
        return 1

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
