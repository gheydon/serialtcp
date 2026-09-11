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
Print the Tool Types out of an Amiga .info (icon) file.

DOS device drivers -- AUX:, SER:, CD0:, RAD: and friends -- keep their whole
configuration in the icon's Tool Types rather than in a text file, which is why
setting one up means "editing the icon". This reads them from the host, so you
can check what a driver icon actually says without booting the Amiga.

Strings inside a .info are stored length-prefixed: a 4-byte big-endian length
followed by that many bytes, NUL-terminated. Validating the prefix is what
separates real Tool Types from icon bitmap data that happens to contain
printable bytes -- a plain `strings` dump shows both and is misleading.

    tooltypes.py FILE.info [FILE.info ...]
"""

import struct
import sys


def extract(data):
    """Every length-prefixed printable string in the file, in order."""
    found = []
    i = 0
    end = len(data) - 4

    while i < end:
        (n,) = struct.unpack_from(">I", data, i)

        # Tool types are short, NUL-terminated, and printable.
        if 2 <= n <= 256 and i + 4 + n <= len(data):
            blob = data[i + 4: i + 4 + n]
            if blob.endswith(b"\0"):
                text = blob[:-1]
                if text and all(32 <= c < 127 for c in text):
                    found.append(text.decode("ascii"))
                    i += 4 + n
                    continue
        i += 2      # .info structures are word-aligned

    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2

    for path in sys.argv[1:]:
        try:
            data = open(path, "rb").read()
        except OSError as e:
            print(f"{path}: {e}")
            continue

        if data[:2] != b"\xe3\x10":
            print(f"{path}: not an Amiga icon (bad magic)")
            continue

        strings = extract(data)
        tools = [s for s in strings if "=" in s and not s.startswith(("IM1", "IM2"))]

        print(f"=== {path} ===")
        if not tools:
            print("    (no tool types)")
        for t in tools:
            print(f"    {t}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
