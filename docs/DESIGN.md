# Design

## The constraint that shapes everything

`bsdsocket.library` hands out a **per-process** context. A socket opened by one
process cannot be used from another, and a device driver runs in the context of
whichever task called it. So a device driver can never touch a socket directly.

Some handoff between the device and a socket-owning process is therefore
mandatory. The only real design question is what to hand off.

## What gets handed off: the whole IORequest

The obvious approach is a shared ring buffer between the device and a daemon,
with signals for wakeups. That means lock-free index handling, careful choices
about 16- versus 32-bit indices (a 32-bit index is not atomic on a 68000, so a
reader can see a torn value), and a separate control channel for everything
that is not data.

This design does none of that, because of one fact about Exec: **a Unit's
MsgPort does not have to be serviced by a task belonging to the device.**

When the daemon attaches to a unit it points that unit's MsgPort straight at
itself:

```c
unit->su_Unit.unit_MsgPort.mp_SigBit  = g_IoSig;
unit->su_Unit.unit_MsgPort.mp_SigTask = FindTask(NULL);   /* the daemon */
unit->su_Unit.unit_MsgPort.mp_Flags   = PA_SIGNAL;
```

Now `BeginIO()` just does `PutMsg()` and the request lands in the daemon's
`Wait()`. It is all one address space, so the daemon reads and writes the
caller's buffers directly and calls `ReplyMsg()` when done.

The consequences are all good ones:

- **No handler task per unit.** Sixteen nodes cost sixteen structs, not sixteen
  tasks with sixteen stacks.
- **No shared ring buffers and no lock-free code.** Buffers live entirely
  inside the daemon and only the daemon touches them.
- **No separate control channel.** `SDCMD_SETPARAMS`, `SDCMD_BREAK` and the
  rest are just IORequests like any other.
- **The device is tiny** — 2896 bytes, or 2412 built with `-DST_MINIMAL`.
  The largest function in it is `open()` at 710 bytes; nothing in it does any
  serial work at all.

The cost is that the daemon must be running for the device to work at all. That
is made explicit rather than hidden: opening a unit with no daemon fails with
`STERR_NO_DAEMON` and prints why.

## Structure

```
BBS (DLG Pro, node 1)          BBS (node 2)
        |                           |
   OpenDevice(serialtcp.device, 0)  OpenDevice(..., 1)
        |                           |
        +---- serialtcp.device -----+        thin shim: allocates units,
                     |                       forwards IO, handles AbortIO
              PutMsg to unit port
                     |
                SerialTCPd                   owns everything real
                     |
   +-----------+-----+------+----------+
   |           |            |          |
 AT engine  telnet     node state   bsdsocket
                        machine         |
                                    listener :23
                                        |
                                    the internet
```

## Call routing

One listening socket serves every node. On `accept()` the daemon walks the node
array and offers the call to the first node that is both **attached** (an
application actually has the unit open) and **on-hook**.

Requiring "attached" matters: a node whose BBS is not running must not swallow
calls into a void. It shows as `closed` in the status client and is skipped.

If no node can take the call, the caller gets the configured busy banner and is
disconnected. That routing-plus-busy behaviour is the main functional
difference from `telser.device`, which binds each unit to its own TCP port and
so requires callers to know which node they want.

## The queue

A caller who cannot be given a node has been `accept()`ed but has no
`struct STNode`, so nothing in the queue path can go through one. That is why
the telnet codec was made independent of nodes: it stages negotiation replies
in the `Telnet` state for the caller to drain, instead of pushing them at a
node's socket. Each waiting caller carries their own small `Telnet` so we stay
in sync with a client that starts negotiating before anyone answers.

**Type-ahead survives the wait.** A merely-waiting caller's socket is never
read, only `MSG_PEEK`ed to notice a hangup, so whatever they type before a node
answers is still in the socket buffer when it does. Only the `ask` prompt
consumes input, and only until they answer it.

Ordering is by arrival time rather than array slot, so freeing a slot in the
middle cannot let a later caller jump the line. Position notices are only sent
when the number actually changes — repeating "you are number 3" every minute is
noise.

`queue_dispatch()` runs after the node sweep in the main loop, so a node that
hung up on this pass is already on-hook and can take the next waiter without an
extra trip round the loop.

The pure parts — the `%N` expander and the arrival-time comparison — live in
`qutil.c` rather than `queue.c`, so the test harness can reach them without
dragging in sockets. Both had bugs waiting to happen: digit order and buffer
bounds in the expander, seconds-versus-microseconds in the comparison.

The `%N` expander is deliberately **not** `printf`. The template comes from a
config file, and handing a user-supplied format string to `printf` is how you
get crashes; a stray `%s` in `queue-position-message` would otherwise be a
remote-ish crash rather than a typo.

## The history graphs

`SerialTCPStat` draws two scrolling graphs with a MUI custom class
(`tools/graph.c`). This is the one place in the project that needs a
register-argument callback, because that is how MUI dispatches to a custom
class — the same `asm("a0")` parameter syntax the device driver uses for its
library vectors.

Samples are stored as raw values with a separate scale rather than pre-computed
pixel heights, so the trace stays correct when the window is resized or when
the node count changes under it. If the class cannot be created the window is
built without the graphs rather than failing: they are a nicety, not the point.

## Things that are easy to get wrong

**Carrier detect is active low.** In `io_Status`, bits 3–7 (DSR, CTS, CD, RTS,
DTR) are asserted when *clear*. DLG Pro reads it as `cd = !(io_Status & (1<<5))`.
Inverting this would give the BBS a permanent carrier and it would never log
anyone off.

**Telnet BINARY is not optional.** Without it, Zmodem transfers corrupt on any
byte that happens to be `0xFF`, and high-bit ANSI art gets mangled. The daemon
negotiates it in both directions and doubles `IAC` in the data stream.

**`+++` must not be forwarded immediately.** A run of plus signs is held back
until the guard time decides whether it is an escape or data. Forwarding
eagerly would corrupt any transfer containing `+++`; swallowing it
unconditionally would lose real data.

**A ringing node must not have its data consumed.** If a caller types ahead
before the BBS answers, those bytes have to stay in the socket buffer — feeding
them to the AT parser would treat the caller's keystrokes as modem commands.
The daemon uses `MSG_PEEK` on ringing nodes purely to notice a caller giving
up.

**A hangup must discard the caller's unread input.** `node_hangup()` clears the
receive buffer as well as the transmit one. Without that, bytes a caller typed
that the application never got round to reading stay buffered across the
hangup and are handed to the application during the *next* caller's session —
one person's keystrokes turning up inside somebody else's login. This was a
real bug, found by live testing rather than by reading the code: the previous
caller's `NO CARRIER` was visibly arriving at the start of the next caller's
session. The clear has to happen *before* the result code is emitted, or
`NO CARRIER` goes out with the stale data.

**Excluding a node from the pool is checked in `node_offer_call()`,** not at
the accept site. Putting it there means the queue honours it for free — a
dial-out-only node is skipped by direct routing and by queue promotion alike,
without either path needing to know the rule exists.

**`Open()` runs under `Forbid()`.** The device has to `Wait()` for the daemon's
reply, which breaks that Forbid, so it holds its own semaphore to keep
Open/Close genuinely single-threaded.

**AbortIO races the daemon.** `AbortIO()` walks the unit port under `Forbid()`:
if the request is still queued it is removed and replied immediately; if the
daemon already dequeued it, a private `io_Flags` bit is set and the daemon is
signalled to notice on its next sweep.

## Memory

| | |
|---|---|
| `struct STNode` | 680 bytes |
| `struct STUnit` | 136 bytes |
| Buffers | 2 × `buffer-size` (default 2048) per node |
| 4 nodes, defaults | ~19 KB total runtime data |

Nodes are allocated for the configured count, not the maximum, and buffers are
separate allocations rather than inline arrays — an earlier draft with
`g_Nodes[32]` and inline 8 KB buffers reserved half a megabyte of BSS before
doing anything.

Every allocation uses `MEMF_ANY` and none use `MEMF_CHIP`. Exec searches its
memory list in priority order and Fast RAM outranks Chip, so on any machine
with Fast RAM all of this lands there and Chip is left for the display.

## Known weak points

- **`gethostbyname()` blocks.** There is no portable asynchronous resolver in
  `bsdsocket.library`, so dialling a hostname stalls every node until the
  lookup returns. Inbound calls and dialling by IP are unaffected. Fixing this
  properly needs a resolver subprocess.
- **The daemon is a single point of failure.** If it dies, in-flight requests
  are failed cleanly rather than hanging, but every session dies with it.
- **No rate limiting.** `answer-baud` is cosmetic.

## Testing

The FIFO, telnet codec and AT parser are pure logic and are compiled natively
on the build host against stub Amiga headers in `tests/fake`, then run:

```bash
make test
```

The suite has been mutation-checked — deliberately breaking the IAC doubling
and the negotiation loop guard each produce exactly one failure — so it is
known to detect regressions rather than merely passing.

What it does **not** cover is anything needing a real Amiga: device open/close,
the IORequest handoff, socket handling and the MUI client are all
compile-verified only.
