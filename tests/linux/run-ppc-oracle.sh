#!/bin/bash
# run-ppc-oracle.sh [build dir] [cases] [seed] [groups]
# Generates random instruction test cases, runs them under qemu-ppc and
# under the emulator core (ppc-exec) and compares the results.
# ORACLE_CPU=603e (default) or 604 picks the processor model of both.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(cd "${1:-$HERE/out}" && pwd)"
COUNT="${2:-20000}"
SEED="${3:-1}"
GROUPS_ARG="${4:-}"
CPU="${ORACLE_CPU:-603e}"
case "$CPU" in
	603e) AS_CPU=-m603 ;;
	604) AS_CPU=-m604 ;;
	*) echo "ORACLE_CPU must be 603e or 604" >&2; exit 2 ;;
esac
WORK="$OUT/oracle-$CPU"
mkdir -p "$WORK"

ARGS=(--seed "$SEED" --count "$COUNT")
[ -n "$GROUPS_ARG" ] && ARGS+=(--groups "$GROUPS_ARG")
python3 -I "$HERE/../../tools/ppc-testgen.py" "${ARGS[@]}" "$WORK/cases.S"

AS=powerpc-linux-gnu-as
LD=powerpc-linux-gnu-ld
$AS -mppc $AS_CPU -o "$WORK/exec.o" "$HERE/ppc/exec.S"
$AS -mppc $AS_CPU -o "$WORK/cases.o" "$WORK/cases.S"
$LD -static -e _start --no-warn-rwx-segments -o "$WORK/exec.elf" \
	"$WORK/exec.o" "$WORK/cases.o"

echo "== instruction oracle ($CPU)"
qemu-ppc -cpu "$CPU" "$WORK/exec.elf" > "$WORK/qemu.bin"
"$OUT/ppc-exec" --model "$CPU" "$WORK/exec.elf" > "$WORK/emu.bin"
python3 -I "$HERE/compare-ppc.py" "$WORK/qemu.bin" "$WORK/emu.bin" \
	"$WORK/cases.S.names" "$WORK/cases.S"
