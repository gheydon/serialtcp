# Testing under FS-UAE

The whole stack has been run end to end on **AmigaOS 3.2 (Kickstart 47.7)**
under FS-UAE, with real TCP connections coming in from the host.

## Why this works without a TCP stack on the Amiga

FS-UAE's `bsdsocket_library = 1` provides an emulated `bsdsocket.library`
backed by the host's network stack. No Roadshow, AmiTCP or Miami install is
needed, and — the useful part — a `listen()` from the Amiga becomes a real
listening socket on the host:

```
$ lsof -nP -iTCP:2323 -sTCP:LISTEN
fs-uae  12718 gordon  TCP *:2323 (LISTEN)
```

So you can telnet from the host straight into the BBS node.

FS-UAE's bsdsocket implementation names its unsupported calls explicitly
(`recvmsg`, `sendmsg`, `getnetbyname`, the `Inet_*` helpers). SerialTCP uses
none of them. The one known `select()` bug in FS-UAE is UDP-specific; this is
all TCP.

## The system under test

A real AmigaOS 3.2 hard drive, built by
[tests/host/build_system.py](../tests/host/build_system.py): the OS files come
from the Hyperion install floppies on the 3.2 CD, merged the way the installer
merges them, with DLG Professional 2.1.4 installed on top and four ports
configured on `serialtcp.device` units 0-3.

Not the CD's Preinstallation Environment. That distinction turned out to
matter: the PE ships a cut-down `LIBS:` with no `diskfont.library`, and DLG's
`SetUp` exits silently without it, so every port reported "activated
successfully" and then did nothing.

Two things the install floppies do not give you, which the script supplies:

- **A hard-drive Startup-Sequence.** The floppies carry the KickDisk version,
  which never runs `S:User-Startup` and reassigns `SYS:` back to the install
  disk at the end.
- **The assigns a real install makes** -- `CLIPS:`, `REXX:`, `PRINTERS:`,
  `KEYMAPS:`, `LOCALE:`, `LIBS: SYS:Classes ADD`, `HELP:`.

SerialTCP itself lives on a separate host-directory drive (`Work:`) so
binaries can be replaced and logs read from the host without rebuilding the
600 MB image.

## Setting it up

1. **Kickstart.** The AmigaOS 3.2 CD carries the ROMs unencrypted in `ROM/` —
   `kicka1200.rom` is Kickstart 3.2 (exec 47.7), 512 KB.

2. **A system to boot.** The 3.2 CD is itself a bootable Preinstallation
   Environment, so copying the CD to a host directory gives a working 3.2
   system without running the installer.

3. **Two host-directory drives**: the OS as `DH0:`, the SerialTCP build as
   `DH1:`. Host directories mean binaries can be dropped in from the host and
   log files read back live while the Amiga runs — no disk image surgery.

```ini
[fs-uae]
amiga_model = A1200
fast_memory = 8192
kickstart_file = /path/to/kicka1200.rom

hard_drive_0 = /path/to/OS32
hard_drive_0_label = AmigaOS3.2
hard_drive_1 = /path/to/SerialTCP
hard_drive_1_label = SerialTCP

bsdsocket_library = 1

# No floppy in this setup, so silence the drive clicking.
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
```

## Driving it without the GUI

Clicking around Workbench through screenshots is slow and fragile. Since `DH0:`
is a host directory, it is far easier to edit the Amiga's `S/Startup-sequence`
from the host and let it do the work:

```
Assign >NIL: DEVS: SerialTCP: ADD
Run >SerialTCP:daemon-out.txt <NIL: SerialTCP:SerialTCPd SerialTCP:serialtcp.conf
Wait 2
Run >SerialTCP:node0.txt <NIL: SerialTCP:SerialTest 0
Run >SerialTCP:node1.txt <NIL: SerialTCP:SerialTest 1
Run >NIL: <NIL: Execute SerialTCP:StatusLoop
```

`Assign DEVS: SerialTCP: ADD` is worth noting: it makes `serialtcp.device`
findable without copying anything into the Workbench install, so the OS
directory stays pristine.

Everything then reports through files in the host directory — the daemon's log,
each node's console, and a status snapshot refreshed every few seconds — all
readable from the host while the Amiga runs.

Two AmigaDOS gotchas that cost time:

- A script run with `Execute` must **not** start with `.key` unless you pass
  arguments; with one, it waits for arguments that never arrive and silently
  does nothing.
- Programs whose output is redirected to a file need unbuffered stdout
  (`setvbuf(stdout, NULL, _IONBF, 0)`) or nothing appears until they exit.

## SerialTest

`SerialTest` (built alongside the rest) stands in for a BBS node: it opens one
unit, waits for carrier, greets the caller, echoes what they type, and reports
carrier changes to the console. It reads carrier exactly the way DLG Pro does —
`io_Status` bit 5, active low — so if it sees carrier correctly, DLG will.

```
SerialTest [unit] [device]
```

It decodes the device's `io_Error` codes on failure, so a failed open says
"SerialTCPd is not running" rather than a bare number.

## What was verified

Everything below was observed on AmigaOS 3.2 under FS-UAE, not inferred:

| | |
|---|---|
| Device opens, both units | `node 0: attached`, `node 1: attached` |
| Telnet negotiation | `IAC WILL ECHO; IAC WILL SGA; IAC WILL BINARY; IAC DO BINARY` |
| Inbound call routing | first free node takes the call |
| RING and auto-answer | `RING` then `CONNECT 38400` delivered to the application |
| Data both directions | greeting out, typed text echoed back |
| Carrier detect | drops on hangup; the node returns to `waiting` |
| Disconnect detection | both nodes noticed in the same second, no stall |
| Busy handling (`ask`) | third caller prompted "Would you like to wait?" |
| Queue | `You are number 1 in the queue` |
| Queue promotion | caller put through automatically when a node freed, `waited 5 seconds, now on node 0` |
| Queue accounting | abandoning the queue counted under "hung up" |
| `SerialTCPStatus` | correct live figures, byte counters, call durations |
| **DLG Pro 2.1.4, four nodes** | opens units 0-3, answers with `ATA`, serves its login screen over telnet |
| Four simultaneous DLG sessions | all four nodes online at once, each with its own login |
| Queue with a real BBS | fifth caller queued, then put through to DLG when a node freed |
| `outbound-only` | node 2 idle but skipped; third caller queued rather than answered |
| Dial-out node display | shows `dial-out`, and `(1 dial-out only)` in the header |
| `queue-lie inflate` | caller told 5, 6, 7, 8, 9 while genuinely first in line |
| Lie does not reorder | caller told "number 7" still served before a later arrival |
| No cross-caller leak | text typed by one caller and never read does not appear in the next caller's session |

Sample of the status client running on the Amiga during two calls and one
queued caller:

```
SerialTCPd  --  up 0m, 2 of 2 nodes busy
listening on 2323   calls 3, turned away busy 0

Node State     Caller               Baud   Time     In      Out
---- --------- -------------------- ------ -------- ------- -------
   0 ONLINE    127.0.0.1             38400 0:08     23      163
   1 ONLINE    127.0.0.1             38400 0:06     23      163

Queue: 1 waiting of 4 places, 1 joined in total
       put through 0, hung up 0, timed out 0, declined 0
```

## A bug only a real BBS could find

Queue promotion originally handed the waiting caller to a node the instant it
hung up. With `SerialTest` that was fine. With DLG it was not:

```
09:42:49 node 0: hung up on 127.0.0.1
09:42:49 node 0: ringing, caller 127.0.0.1      <- queued caller promoted
09:42:50 node 0: hung up on 127.0.0.1           <- dropped one second later
09:42:50 node 0: detached (unit closed)         <- DLG was still tearing down
09:42:52 node 0: attached (unit opened)
```

A BBS does not finish with a line the moment carrier drops. DLG tears the
session down and *closes and reopens the unit* while doing it, so a caller
handed over inside that window is connected and then dropped — and for a
queued caller that happens at exactly the moment they are finally served.

`node-settle` (default 2 seconds, 3 in the test config) makes a node wait after
becoming free before it is offered another call. The clock restarts both on
hangup and on re-attach, which covers BBSes that close the device between
calls and those that keep it open. Afterwards:

```
09:44:59 node 0: hung up on 127.0.0.1
09:45:00 node 0: detached (unit closed)
09:45:02 node 0: attached (unit opened)
09:45:06 node 0: ringing, caller 127.0.0.1      <- waited, then promoted
09:45:08 node 0: online with 127.0.0.1
```

and the caller gets DLG's login screen.

## Still untested

- **`SerialTCPStat` (the MUI client).** AmigaOS 3.2 does not ship MUI, so it
  has not been run. The CLI client exercises the same protocol code, but the
  MUI window, the history graphs and the Start/Stop/Restart buttons have only
  been compiled, never displayed.
- **Real hardware.** FS-UAE's emulated bsdsocket is not Roadshow. The socket
  calls used are ordinary BSD ones, but a real stack may differ in timing and
  in how `WaitSelect` interacts with Exec signals.

## Note on the idle byte counters

An idle node keeps showing the byte totals from its last call rather than
resetting to zero. That is deliberate — knowing what the previous caller
transferred is useful — but the column header does not say so.
