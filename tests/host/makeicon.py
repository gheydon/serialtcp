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
Generate an Amiga .info (icon) file.

The artwork is written as ASCII art below and converted to the planar bitmap
format Intuition wants: one bitplane per bit of colour depth, each row padded
to a 16-bit boundary. Four colours, using the standard Workbench palette
(0 grey, 1 black, 2 white, 3 blue), which is what every 3.x system has.

Writing the DiskObject by hand rather than shipping a binary blob means the
icon is readable and editable in this file, and reviewable in a diff.

    makeicon.py <output.info> [--tooltype KEY=VALUE ...]
"""

import struct
import sys

# 0 = background (transparent grey), 1 = black, 2 = white, 3 = blue
PALETTE = {" ": 0, "#": 1, ".": 2, "*": 3}

# A modem with three status LEDs, and a signal arcing away from it.
ART = [
    "                                              ",
    "                                   *     *    ",
    "                              *   * *   * *   ",
    "                         *   * *  * *   * *   ",
    "                        * *  * *  * *   * *   ",
    "  ####################################  * *   ",
    "  #..................................#  * *   ",
    "  #.################################.#  * *   ",
    "  #.#******************************#.#  * *   ",
    "  #.#*                            *#.#  * *   ",
    "  #.#*                            *#.#  * *   ",
    "  #.#******************************#.#  * *   ",
    "  #.################################.#  * *   ",
    "  #..................................#  * *   ",
    "  #.  ##    ##    ##                .#  * *   ",
    "  #.  ##    ##    ##                .#  *     ",
    "  #..................................#        ",
    "  ####################################        ",
    "                                              ",
]

WBTOOL = 3
NO_ICON_POSITION = 0x80000000
GFLG_GADGIMAGE = 0x0004
GACT_RELVERIFY = 0x0001
GACT_IMMEDIATE = 0x0002
BOOLGADGET = 0x0001


def planar(art, depth=2):
    """ASCII art -> one byte string per bitplane, rows padded to 16 bits."""
    height = len(art)
    width = max(len(r) for r in art)
    words = (width + 15) // 16
    planes = []

    for plane in range(depth):
        data = bytearray()
        for row in art:
            row = row.ljust(width)
            bits = []
            for x in range(words * 16):
                c = row[x] if x < width else " "
                colour = PALETTE.get(c, 0)
                bits.append((colour >> plane) & 1)
            for i in range(0, len(bits), 8):
                byte = 0
                for b in bits[i:i + 8]:
                    byte = (byte << 1) | b
                data.append(byte)
        planes.append(bytes(data))

    return width, height, words, planes


def build(tooltypes):
    width, height, words, planes = planar(ART)
    depth = len(planes)
    image_data = b"".join(planes)

    # struct Gadget, 44 bytes. The render pointers only need to be non-zero:
    # icon.library replaces them with real addresses when the icon is loaded.
    gadget = struct.pack(
        ">IhhhhHHHIIIiIHI",
        0,                      # ga_NextGadget
        0, 0,                   # ga_LeftEdge, ga_TopEdge
        width, height,          # ga_Width, ga_Height
        GFLG_GADGIMAGE,         # ga_Flags
        GACT_RELVERIFY | GACT_IMMEDIATE,
        BOOLGADGET,
        1,                      # ga_GadgetRender -- present
        0,                      # ga_SelectRender -- none, complement on click
        0,                      # ga_GadgetText
        0,                      # ga_MutualExclude
        0,                      # ga_SpecialInfo
        0,                      # ga_GadgetID
        0,                      # ga_UserData
    )
    assert len(gadget) == 44, len(gadget)

    disk_object = (
        struct.pack(">HH", 0xE310, 1)
        + gadget
        + struct.pack(">BB", WBTOOL, 0)
        + struct.pack(">I", 0)                      # do_DefaultTool (tools: unused)
        + struct.pack(">I", 1 if tooltypes else 0)  # do_ToolTypes
        + struct.pack(">II", NO_ICON_POSITION, NO_ICON_POSITION)
        + struct.pack(">I", 0)                      # do_DrawerData
        + struct.pack(">I", 0)                      # do_ToolWindow
        + struct.pack(">i", 8192)                   # do_StackSize
    )
    assert len(disk_object) == 78, len(disk_object)

    image = struct.pack(
        ">hhhhhIBBI",
        0, 0,                   # LeftEdge, TopEdge
        width, height, depth,
        1,                      # ImageData -- present
        (1 << depth) - 1,       # PlanePick
        0,                      # PlaneOnOff
        0,                      # NextImage
    )
    assert len(image) == 20, len(image)

    out = disk_object + image + image_data

    if tooltypes:
        # Length-prefixed, NUL-terminated, with a count word first.
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            b = t.encode("latin-1") + b"\0"
            out += struct.pack(">I", len(b)) + b

    return out, width, height, depth


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2

    path = sys.argv[1]
    tooltypes = []
    args = sys.argv[2:]
    while args:
        a = args.pop(0)
        if a == "--tooltype" and args:
            tooltypes.append(args.pop(0))

    data, w, h, d = build(tooltypes)
    with open(path, "wb") as f:
        f.write(data)

    print(f"{path}: {w}x{h}, {d} planes, {len(data)} bytes"
          + (f", {len(tooltypes)} tool types" if tooltypes else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
