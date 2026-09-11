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
A small interactive telnet client, for calling a SerialTCP node from the host.

macOS has shipped without /usr/bin/telnet since High Sierra, and `nc` is no
substitute here: it does not answer option negotiation, so the session opens
with a spray of IAC bytes and stays in the wrong mode for ANSI or Zmodem.

This answers negotiation properly, agrees to 8-bit binary in both directions
(which is what a BBS needs), and puts the terminal in raw mode so keystrokes
reach the Amiga one at a time.

    tests/host/telnet.py [host] [port]        default 127.0.0.1 2323

Ctrl-]  quits.  Ctrl-C is passed through to the remote end, not to this client,
so it reaches the BBS the way it would over a real modem.
"""

import socket
import sys
import select
import termios
import tty

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
BINARY, ECHO, SGA = 0, 1, 3

QUIT_KEY = 0x1D          # Ctrl-]


class Telnet:
    def __init__(self, sock):
        self.s = sock
        self.buf = bytearray()
        self.state = "data"
        self.sub = bytearray()
        # Options we are willing to turn on. Everything else is refused.
        self.local = {BINARY: False, SGA: False}
        self.remote = {BINARY: False, SGA: False, ECHO: False}

    def send_cmd(self, verb, opt):
        self.s.sendall(bytes([IAC, verb, opt]))

    def offer(self):
        """Opening offer: ask for 8-bit clean both ways."""
        self.send_cmd(WILL, BINARY)
        self.send_cmd(DO, BINARY)
        self.send_cmd(DO, SGA)

    def _do(self, opt):
        if opt in self.local:
            if not self.local[opt]:
                self.local[opt] = True
                self.send_cmd(WILL, opt)
        else:
            self.send_cmd(WONT, opt)

    def _dont(self, opt):
        if self.local.get(opt):
            self.local[opt] = False
            self.send_cmd(WONT, opt)

    def _will(self, opt):
        if opt in self.remote:
            if not self.remote[opt]:
                self.remote[opt] = True
                self.send_cmd(DO, opt)
        else:
            self.send_cmd(DONT, opt)

    def _wont(self, opt):
        if self.remote.get(opt):
            self.remote[opt] = False
            self.send_cmd(DONT, opt)

    def feed(self, data):
        """Wire bytes in, application bytes out."""
        out = bytearray()

        for c in data:
            st = self.state

            if st == "data":
                if c == IAC:
                    self.state = "iac"
                else:
                    out.append(c)

            elif st == "iac":
                if c == IAC:
                    out.append(IAC)
                    self.state = "data"
                elif c in (DO, DONT, WILL, WONT):
                    self.state = {DO: "do", DONT: "dont", WILL: "will", WONT: "wont"}[c]
                elif c == SB:
                    self.sub.clear()
                    self.state = "sb"
                else:
                    self.state = "data"

            elif st in ("do", "dont", "will", "wont"):
                {"do": self._do, "dont": self._dont,
                 "will": self._will, "wont": self._wont}[st](c)
                self.state = "data"

            elif st == "sb":
                if c == IAC:
                    self.state = "sb_iac"
                else:
                    self.sub.append(c)

            elif st == "sb_iac":
                # Subnegotiations are consumed and ignored; consuming them
                # correctly is what keeps the stream in sync.
                self.state = "data" if c == SE else "sb"

        return bytes(out)

    def write(self, data):
        """Application bytes out, IAC doubled so 0xFF survives."""
        self.s.sendall(data.replace(b"\xff", b"\xff\xff"))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 2323

    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as e:
        print(f"Could not connect to {host}:{port} -- {e}")
        return 1

    print(f"Connected to {host}:{port}.  Ctrl-] to quit.\r", flush=True)
    sock.settimeout(None)

    tn = Telnet(sock)
    tn.offer()

    stdin_fd = sys.stdin.fileno()
    isatty = sys.stdin.isatty()
    saved = termios.tcgetattr(stdin_fd) if isatty else None

    # When stdin is a pipe rather than a terminal it hits EOF immediately.
    # Quitting there would make the client useless in a script, so instead we
    # stop watching stdin and keep reading the remote until it closes or goes
    # quiet.
    stdin_open = True
    idle_limit = None if isatty else 5.0

    try:
        if isatty:
            tty.setraw(stdin_fd)

        while True:
            watch = [sock] + ([sys.stdin] if stdin_open else [])
            r, _, _ = select.select(watch, [], [], idle_limit)

            if not r:
                break               # idle timeout (scripted use only)

            if sock in r:
                data = sock.recv(4096)
                if not data:
                    break
                out = tn.feed(data)
                if out:
                    sys.stdout.buffer.write(out)
                    sys.stdout.buffer.flush()

            if stdin_open and sys.stdin in r:
                key = sys.stdin.buffer.raw.read(1) if isatty else sys.stdin.buffer.read(1)
                if not key:
                    stdin_open = False
                    continue
                if key[0] == QUIT_KEY:
                    break
                tn.write(key)

    finally:
        if isatty:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        sock.close()

    print("\r\nDisconnected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
