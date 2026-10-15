#!/bin/bash
# disasm.sh FILE [START [END [VMA_OF_FILE_START]]]
# Disassembles a big endian PowerPC binary (a ROM image, a memory dump)
# with the cross toolchain's objdump. START and END are addresses (hex) as
# the processor sees them; VMA_OF_FILE_START is the address of the file's
# first byte (default 0xFFC00000, the Power Mac's 4 MB ROM).
#   tools/disasm.sh 9500.rom 0xfff03000 0xfff03200
set -e
FILE=${1:?usage: disasm.sh FILE [START [END [VMA]]]}
VMA=${4:-0xffc00000}
ARGS=(-D -b binary -m powerpc:common -EB --adjust-vma="$VMA")
[ -n "${2:-}" ] && ARGS+=(--start-address="$2")
[ -n "${3:-}" ] && ARGS+=(--stop-address="$3")
powerpc-linux-gnu-objdump "${ARGS[@]}" "$FILE" | tail -n +8
