#!/bin/sh
# Build SerialTCP inside the m68k-amigaos cross-compiler container.
# Usage: ./build.sh [make targets...]
IMAGE=amigadev/crosstools:m68k-amigaos
exec docker run --rm -v "$(pwd)":/work -w /work "$IMAGE" make "$@"
