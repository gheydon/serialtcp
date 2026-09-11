# Setting up DLG Professional

DLG Pro talks to whatever serial device you name in its Port Configuration
editor, so pointing it at `serialtcp.device` is a configuration change, not a
patch. Nothing in DLG needs modifying.

## What DLG actually needs from a serial device

Taken from DLG's own source (`Handler/Handler/Serial.c` and `Main.c`), the
handler:

- opens the device with `io_SerFlags` preset to
  `SERF_SHARED | SERF_XDISABLED | SERF_RAD_BOOGIE`, adding `SERF_7WIRE` when
  the port is configured for 7-wire handshaking;
- issues `SDCMD_SETPARAMS` with 8 data bits, 8 stop-bit field, `io_BrkTime`
  250000;
- polls `SDCMD_QUERY` for the byte count and for carrier;
- detects carrier with `cd = !(io_Status & (1 << 5))` — bit 5, active low,
  retried five times before it believes a loss;
- uses asynchronous `CMD_READ` / `CMD_WRITE` with `AbortIO`, and `SDCMD_BREAK`.

`serialtcp.device` implements all of that, including the active-low carrier
convention, which is the detail that decides whether DLG ever logs a caller
off.

## Configuration

In the DLG configuration editor, choose **[P] Port Configuration**. DLG ships
with two ports: `TR0` (remote) and `TL0` (local). For each remote node:

| Field | Set to |
|---|---|
| Serial Device | `serialtcp.device` |
| Unit Number | `0` for the first node, `1` for the second, and so on |
| Modem File | your modem definition (see below) |
| Global Set | as before |
| Display File | as before |

Leave `TL0` alone — it uses `console.device` and is the local login.

For a four-node system, create ports `TR0` to `TR3` with units 0 to 3, and set
`nodes 4` in `S:serialtcp.conf`. Unit numbers must match: node *n* uses unit
*n*.

Device names are case sensitive in DLG. Type `serialtcp.device` exactly.

## Modem file

Create a modem definition (or copy an existing one) with:

| | |
|---|---|
| Init string | `AT&F&C1&D2E0Q0V1S0=1` |
| Answer string | `ATA` |
| Hangup string | `ATH` |
| Connect message | `CONNECT` |
| Baud | `38400` |

`S0=1` makes the virtual modem answer on the first ring so DLG never has to.
If you would rather DLG answer calls itself, use `S0=0` and let it send `ATA`
when it sees `RING` — both work.

`&C1` matters: it tells the modem that carrier detect should follow the real
carrier, which is what lets DLG notice a caller hanging up. Do not use `&C0`.

## Hang Up with DTR — set this to NO

This one matters, and it is not obvious.

DLG offers two hangup methods (`struct Modem.HangupMethod` in
`dlg/portconfig.h`), and `Login/hangup.c` shows what each does:

- **Command method** (`Hang Up with DTR = NO`) — DLG sends the Return-to-
  Command-Mode string (`+++`), waits, then the Hang Up string (`ATH0`). Both
  are implemented here, including `+++` guard timing, so this works.
- **DTR method** (`Hang Up with DTR = YES`) — `hangup.c` simply exits and
  relies on DTR dropping.

`serial.device` has no DTR control command on the Amiga: DTR is asserted for as
long as the device is open, and there is no way for a driver to observe an
application "toggling" it. So set **Hang Up with DTR to NO** and let DLG use
`+++` / `ATH0`.

This only affects DLG hanging up on a caller. The other direction always works:
when a caller disconnects, the daemon drops carrier and DLG's `CarrierDetect()`
notices within its usual five-poll window. Closing the unit also hangs up, so
shutting a node down never leaves a caller connected.

## How DLG decides there is no modem

Short answer: it does not probe. There is no modem-handshake code anywhere in
the DLG source tree — no AT strings, no response parsing. DLG sends the Init
String from the modem file and simply carries on.

What DLG actually watches for is:

- the **Ring String** (`RING` by default) arriving on the port, and
- **carrier detect**, via `SDCMD_QUERY` bit 5.

So with no modem attached, a DLG node does not report an error — it sits
waiting for a `RING` that never comes. That is worth knowing when debugging:
if a node shows `waiting` in `SerialTCPStat` and nothing happens when you
telnet in, the problem is upstream of DLG.

Since the daemon emits a literal `RING` and drives carrier detect properly,
DLG sees exactly what it expects from a real modem.

## BBS Answer Mode and TrapDoor

The modem file's **BBS Answer Mode** decides who picks up the line. Set it to
YES for a normal BBS node and let DLG (or `S0=1`) answer.

Set it to NO if a front-end mailer such as TrapDoor answers the line and hands
sessions to DLG — DLG has explicit TrapDoor support (`DLGMail` accepts
`TRAPDOOR ON|OFF|RECONFIG|ANSWER|NOANSWER`). That arrangement still works here:
TrapDoor would open a `serialtcp.device` unit itself and see a normal modem.
Whether FidoNet-over-telnet is useful to you is another question — binkd-style
transports have largely replaced dial-up mailer sessions.

## Starting up

Order matters. In `S:User-Startup`, or wherever you start things:

1. Your TCP/IP stack (Roadshow, AmiTCP, Miami).
2. `Run >NIL: SerialTCPd`
3. DLG.

If DLG starts before the daemon, opening the device fails and the node will not
come up — the daemon is what actually owns the units. Restart that node after
starting the daemon.

## Checking it works

With the daemon running but before starting DLG, `SerialTCPStat` should show
every node as `closed` — nothing has the units open yet. Once DLG's nodes are
running they turn to `waiting`. Telnet in from another machine and one should
go `RINGING` then `ONLINE`.

If all nodes show `closed` while DLG is running, DLG is not opening the device:
check the device name spelling and the unit numbers in Port Configuration.

## Notes

- **Zmodem works.** The daemon negotiates telnet BINARY in both directions and
  escapes `0xFF` correctly, which is what Zmodem needs to survive a telnet
  link.
- **`answer-baud` is cosmetic.** Transfers run at network speed regardless of
  what DLG thinks the port speed is. Set DLG's baud to 38400 and leave it.
- **7-wire handshaking is accepted and ignored.** There is no hardware to
  handshake with; the daemon buffers instead, so it makes no difference either
  way.

## Installing DLG under emulation, the hard-won bits

A DLG install was assembled and booted against this driver on AmigaOS 3.2.
[tests/host/install_dlg.py](../tests/host/install_dlg.py) does it from the host
rather than clicking through `Disk1:Install_DLG`, which makes it repeatable and
lets the four port definitions be generated exactly instead of typed into the
config editor four times.

Four things cost real time and are worth knowing:

**`diskfont.library` must be present.** `SetUp` -- the program run on each port
-- opens it and exits if it cannot:

```c
if (!(DiskfontBase = OpenLibrary("diskfont.library", 33L)))
    CleanUp("Unable to open diskfont.library");
```

The AmigaOS 3.2 CD's Preinstallation Environment ships a minimal `Libs` with no
`diskfont.library`. Until it was added, every port reported "activated
successfully" and then silently did nothing. A full hard-drive install has it.

**`TPTCron` is not optional.** `ActivatePort` does not launch ResMan itself --
it asks the cron daemon to, and then polls:

```c
AFPrintf(NULL, sout, " Installing ResMan ...\n\n");
CronEvent(ADDEVENT, 0, "DLG:ResMan");
for (count = 0; count < 100; count++) { Delay(50); if (FindPort(RMCONTROL)) break; }
```

Leave `TPTCron` out of your startup and the boot hangs on
`Installing ResMan ...` for a hundred seconds and then gives up. It also wants
`NULL:` mounted, since that is where it logs.

**Mount does not accept a comment before the first mountlist entry.** A
`; ...` line at the top of `DEVS:TPTMountlist` produces
`ERROR: Device 'TL0:' not found in file 'Devs:TPTMountlist'`, which reads like
a missing entry rather than a parse problem. The shipped mountlist puts its
comments after every entry, and so must yours.

**One side answers, never both.** With the modem set to auto-answer
(`auto-answer 1` here), DLG's Answer String is ignored -- the DLG manual says
as much. Configure one or the other to pick up the line.

## Open problem: only TR0 recycles

**Symptom.** After the first session on a node, DLG closes its unit and does
not reopen it. Only `TR0` comes back. On a long run, a four-node board quietly
becomes a one-node board.

**It is the port number, not the order.** With `TR0` held busy so a second
caller landed on `TR1`:

```
node 0: detached  ->  attached   3s later
node 1: detached  ->  never came back
node 0: detached  ->  attached   2s later   (cycled again, fine)
```

**Proved not to be the driver, by swapping the units under the ports.**
Normally `TR0` is on unit 0, so "the port that survives" and "unit 0" cannot
be told apart. Reconfiguring `TR0` onto unit 1 and `TR1` onto unit 0
separates them:

```
node 0: detached   (unit 0, now TR1)   never came back
node 2: detached   (unit 2, TR2)       never came back
node 1: detached   (unit 1, now TR0)   attached 2s later
```

Unit 0 -- previously the one that always recovered -- now dies, and unit 1
recovers in its place, because `TR0` moved there. The survivor follows the
DLG port name, not the device unit. Whatever is special about `TR0` is
entirely inside DLG.

**It is also not the driver by inspection.** The `detached` line is logged from inside
`node_detach()`, which clears `su_Attached`, so a reopen would be accepted --
and a successful open always logs `attached`. The absence of that line is
evidence that DLG never called `OpenDevice()` again, rather than that the call
failed. `TR0` reopening repeatedly on the same driver makes the same point.

**What DLG thinks is happening.** Re-running `ActivatePort` on a stopped port
gives:

```
Error: Port is already active
```

So ResMan still has the port marked active; it has not noticed that the
`SetUp` process on that port is gone. `DeactivatePort` on such a port does not
return at all -- it appears to wait for a port that will never come free -- so
the obvious watchdog (deactivate, then activate) hangs rather than helping.

**The port lifecycle, from the source and from the running machine.**
`SetUp` is what waits for carrier on a port. It is never invoked by name
anywhere in DLG's source, so the only thing that ever starts it is ResMan
running the command registered by `ActivatePort`. When a call arrives the
session chain takes over, and `SetUp` exits through `CleanUp()`:

```c
if (!spawned)      FreePort(port, "Setup");
exit(s?5:0);
```

`FreePort` is what hands the port back so ResMan can start the next `SetUp`.
A process dump from the running machine matches: at idle there is one
`DLG:SetUp` per remote port; during a call the busy port's `SetUp` is replaced
by `DLG:door`; after the call, `TR0` gets a fresh `SetUp` and the others do
not.

So the failure is that ResMan does not start the next `SetUp` on those ports.
Whether `FreePort` is not being reached, or is reached and ResMan does not act
on it, is not established -- it would need either a careful read of `RM/` or a
debugger on the Amiga.

**Unresolved.** Why `TR0` is special is not established. Worth noting that
`Disk1:Install/start.2` only ever mounts and activates `TR0`, with a comment
saying *"If you add more ports, you will need to add more ActivatePort
commands for them"* -- so multi-node was a documented-but-less-travelled path.
A licence limit is one candidate but a weak one: `Handler/Handler/Main.c`
checks the serial number and includes `<dialog/dead.h>` inside that check, and
`dead.h` is one of the files missing from the released source, so that part
cannot be read -- but nothing in the licence terms mentions a node count, and
`BadFlag` gates output rather than port restarts. Ports here serve their
first call correctly, which is not what a node limit would look like.

If you know DLG well, this is the place to look -- from the driver's side
everything needed for the reopen is in place and waiting.

## Licence note on DLG itself

DLG Professional was released as freeware by Jeff Grimmett, with source, in
October 2010 on Aminet (`comm/dlg/DLG_Source.zip`). The author's terms:

> I have released DLG as freeware, and the source code is available for private
> or public use on the Amiga platform as long as it is not a commercial
> product.

Commercial development on Linux or any form of Unix is expressly denied —
Grimmett cites pre-existing agreements. Windows development is explicitly
permitted. None of this constrains SerialTCP, which is Amiga-only,
non-commercial, and independently written; DLG's source was read only to
determine which serial device calls it makes.
