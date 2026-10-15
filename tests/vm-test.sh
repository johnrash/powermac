#!/bin/bash
# ROM=FILE CARD_ROM=FILE TEST_DISK=FILE [MACOS_CD=FILE] [OUT=DIR] \
#     vm-test.sh user@haiku-host
#
# End-to-end tests of the PowerMac app on a Haiku machine, run from Linux:
# syncs and builds the app there, copies the ROMs and a test disk, drives
# the app with hey (its scripting properties and commands) and checks the
# replies, screenshots and the saved settings. The app's settings folder on
# the Haiku machine is backed up and restored.
#
# Needs here: the 4 MB ROM (ROM), the video card ROM (CARD_ROM) and a disk
# image with Mac OS 7.6.1 installed (TEST_DISK); for the CD test the Mac OS
# 7.6.1 CD image (MACOS_CD). The test disk is a copy on the Haiku machine;
# the original here is never written. The floppy test uses a blank image
# made there, the shared Mac folder test a folder made there. Screenshots
# go to OUT (a temporary folder unless given).
set -u
USAGE="usage: ROM=FILE CARD_ROM=FILE TEST_DISK=FILE [MACOS_CD=FILE] \
vm-test.sh user@haiku-host"
VM=${1:?$USAGE}
: "${ROM:?$USAGE}" "${CARD_ROM:?$USAGE}" "${TEST_DISK:?$USAGE}"
MACOS_CD=${MACOS_CD:-}
OUT=${OUT:-${TMPDIR:-/tmp}/powermac-vm-test}
mkdir -p "$OUT"
# the files as given from here, before going to the source tree
ROM=$(realpath "$ROM") CARD_ROM=$(realpath "$CARD_ROM")
TEST_DISK=$(realpath "$TEST_DISK") OUT=$(realpath "$OUT")
[ -n "$MACOS_CD" ] && MACOS_CD=$(realpath "$MACOS_CD")
cd "$(dirname "$0")/.."
APP_DIR=PowerMac
TEST_DIR=/boot/home/PowerMac/test

PASSED=0
FAILED=0

# a command on the Haiku machine (a hung hey fails, it does not stall)
vm() {
	timeout 300 ssh -o BatchMode=yes "$VM" "$1"
}

cleanup() {
	vm 'hey -s PowerMac quit >/dev/null 2>&1; sleep 2;
		S=~/config/settings/PowerMac; rm -rf "$S";
		[ -d "$S.vmtest-backup" ] && mv "$S.vmtest-backup" "$S"; true' \
		>/dev/null 2>&1
}
trap cleanup EXIT

result() {
	if [ "$2" = ok ]; then
		PASSED=$((PASSED + 1))
	else
		FAILED=$((FAILED + 1))
	fi
	printf "%-36s %s\n" "$1" "$2"
}

check() {
	if [ "$2" = "$3" ]; then
		result "$1" ok
	else
		result "$1" "FAILED (expected \"$2\", got \"$3\")"
	fi
}

# check_at_least NAME MIN VALUE (numbers, decimals allowed)
check_at_least() {
	if awk -v v="$3" -v m="$2" 'BEGIN { exit !(v + 0 >= m + 0) }'; then
		result "$1" ok
	else
		result "$1" "FAILED (expected >= $2, got \"$3\")"
	fi
}

# get PROPERTY [SPECIFIER...]: first "result" of the machine window
get() {
	vm "hey PowerMac get $* of Window 0" \
		| sed -n 's/.*"result" ([A-Z_0-9]*) : //p' | head -1 \
		| sed 's/^"\(.*\)"$/\1/; s/ (0x[0-9a-fA-F]*)$//; s/^TRUE$/1/;
			s/^FALSE$/0/'
}

# shot NAME: the emulated screen into $OUT/NAME.png
shot() {
	vm "rm -f /tmp/powermac-$1.png; hey PowerMac shot of Window 0 with \
		path=/tmp/powermac-$1.png" >/dev/null
	rm -f "$OUT/$1.png"
	scp -q -o BatchMode=yes "$VM:/tmp/powermac-$1.png" "$OUT/$1.png"
}

# wait_emulated SECONDS: until the machine has run that long
wait_emulated() {
	for i in $(seq 1 120); do
		t=$(get EmulatedTime)
		awk -v t="${t:-0}" -v m="$1" 'BEGIN { exit !(t + 0 >= m + 0) }' \
			&& return 0
		sleep 2
	done
	return 1
}

start_app() {
	vm "cd ~/$APP_DIR && (./objects.*-release/PowerMac $* \
		> /tmp/powermac-vmtest.log 2>&1 &) ; waitfor 'w>PowerMac'; sleep 1"
}

quit_app() {
	vm 'hey -s PowerMac quit; for i in 1 2 3 4 5; do
		roster | grep -q PowerMac-Emulator || break; sleep 1; done'
}

echo "== build on $VM"
# tar over ssh (Haiku has no rsync); "x m" leaves the times to the Haiku
# machine, so a clock behind ours does not make make loop
tar cf - --exclude='objects*' Makefile PowerMac.rdef artwork src \
	| ssh -o BatchMode=yes "$VM" "mkdir -p $APP_DIR && cd $APP_DIR \
		&& tar xmf -" || exit 1
vm "cd ~/$APP_DIR && make -j4 2>&1 | grep -E 'error|warning:' \
	| grep -v 'valid for C++'; true"
vm "mkdir -p $TEST_DIR"
vm 'hey -s PowerMac quit >/dev/null 2>&1; sleep 1;
	S=~/config/settings/PowerMac; rm -rf "$S.vmtest-backup";
	[ -d "$S" ] && mv "$S" "$S.vmtest-backup"; true'
scp -q -o BatchMode=yes "$ROM" "$VM:$TEST_DIR/9500.rom"
scp -q -o BatchMode=yes "$CARD_ROM" "$VM:$TEST_DIR/tt128.rom"
scp -q -o BatchMode=yes "$TEST_DISK" "$VM:$TEST_DIR/macos.qcow2"
ROMS="--rom $TEST_DIR/9500.rom --card-rom $TEST_DIR/tt128.rom"
PROFILE='~/config/settings/PowerMac/Profiles/Default'
vm "rm -rf $TEST_DIR/share; mkdir $TEST_DIR/share
	echo 'Hello from Haiku' > $TEST_DIR/share/Hello.txt"

echo "== power and status"
start_app $ROMS --scsi-int-0-disk $TEST_DIR/macos.qcow2 \
	--scsi-int-1-share-mac $TEST_DIR/share
check "starts powered off" 0 "$(get Running)"
check "first profile is Default" Default "$(get Profile)"
vm 'hey -s PowerMac set Running of Window 0 to true'
check "Running set true" 1 "$(get Running)"
wait_emulated 40
check_at_least "emulated time" 40 "$(get EmulatedTime)"
check_at_least "speed (percent)" 30 "$(get Speed)"
check_at_least "MIPS" 20 "$(get Mips)"
check "DiskActivity readable" ok \
	"$(get DiskActivity | grep -q -E '^[01]$' && echo ok)"
check "system on" 1 "$(get SystemOn)"
check "Shared Mac volume built" ok "$(vm "test -s \
	$PROFILE/shared-mac-int1.qcow2 && echo ok")"

echo "== Mac OS: screen, typing, pointer"
shot energy-saver
size=$(stat -c %s "$OUT/energy-saver.png" 2>/dev/null || echo 0)
check_at_least "screenshot PNG size" 2000 "$size"
# Return closes the Energy Saver message of a first start
vm "hey PowerMac type of Window 0 with text=\$'\\n'" >/dev/null
sleep 6
shot finder
if [ ! -s "$OUT/finder.png" ]; then
	result "typed Return reaches Mac OS" "FAILED (no screenshot)"
elif cmp -s "$OUT/energy-saver.png" "$OUT/finder.png"; then
	result "typed Return reaches Mac OS" "FAILED (screen unchanged)"
else
	result "typed Return reaches Mac OS" ok
fi
pointer=$(vm "hey PowerMac get Pointer of Window 0" \
	| sed -n 's/.*BPoint(\([-0-9.]*\), \([-0-9.]*\)).*/\1 \2/p')
check "Pointer is on the screen" ok "$(echo "$pointer" | awk '{
	print ($1 >= 0 && $1 < 1024 && $2 >= 0 && $2 < 768) ? "ok" : "off: " $0 }')"
check "SerialLog 1 (console)" ok "$(vm "hey PowerMac get SerialLog 1 of \
	Window 0" | grep -q '"error" (B_INT32_TYPE) : 0' && echo ok)"

if [ -f "$MACOS_CD" ]; then
	echo "== CD-ROM drive"
	vm "test -s $TEST_DIR/macos.iso" || scp -q -o BatchMode=yes "$MACOS_CD" \
		"$VM:$TEST_DIR/macos.iso"
	vm "hey -s PowerMac set Cd of Window 0 to $TEST_DIR/macos.iso"
	check "Cd inserted" $TEST_DIR/macos.iso "$(get Cd)"
	sleep 20
	shot cd-mounted
	if cmp -s "$OUT/finder.png" "$OUT/cd-mounted.png"; then
		result "Mac OS mounts the CD" "FAILED (screen unchanged)"
	else
		result "Mac OS mounts the CD" ok
	fi
else
	echo "skip CD test (no MACOS_CD)"
fi

echo "== floppy drive"
# a blank disk: Mac OS offers to initialize it, Return (Eject) ejects it
vm "dd if=/dev/zero of=$TEST_DIR/floppy.img bs=512 count=2880 2>/dev/null"
shot before-floppy
vm "hey -s PowerMac set Floppy of Window 0 to $TEST_DIR/floppy.img"
check "Floppy inserted" $TEST_DIR/floppy.img "$(get Floppy)"
sleep 10
shot floppy
if cmp -s "$OUT/before-floppy.png" "$OUT/floppy.png"; then
	result "Mac OS sees the floppy" "FAILED (screen unchanged)"
else
	result "Mac OS sees the floppy" ok
fi
vm "hey PowerMac type of Window 0 with text=\$'\\n'" >/dev/null
sleep 6
check "Mac OS ejected the floppy" "" "$(get Floppy)"

echo "== shared Mac folder: Mac OS shuts down, the folder syncs"
# the power key's dialog; Return is Shut Down
vm "hey -s PowerMac pkey of Window 0"
sleep 4
vm "hey PowerMac type of Window 0 with text=\$'\\n'" >/dev/null
for i in $(seq 1 30); do
	[ "$(get Running)" = 0 ] && break
	sleep 2
done
check "Mac OS powered the machine off" 0 "$(get Running)"
check "Shared Mac volume synced" ok "$(vm "test ! -e \
	$PROFILE/shared-mac-int1.qcow2 && echo ok")"
check "only the folder's files came back" Hello.txt \
	"$(vm "ls $TEST_DIR/share")"
if [ -f "$MACOS_CD" ]; then
	# Mac OS ejects CDs as it shuts down; the CD goes back for the
	# settings test
	check "Mac OS ejected the CD at shut down" "" "$(get Cd)"
	vm "hey -s PowerMac set Cd of Window 0 to $TEST_DIR/macos.iso"
fi

echo "== restart holding Command-Option-O-F: Open Firmware"
vm "hey -s PowerMac rhld of Window 0 with keys=Command+Option+O+F \
	with seconds=8"
sleep 30
log=$(vm "hey PowerMac get SerialLog 1 of Window 0")
check "Open Firmware banner" ok \
	"$(echo "$log" | grep -q 'Open Firmware' && echo ok)"
check "Open Firmware prompt" ok "$(echo "$log" | grep -q ' ok' && echo ok)"

echo "== settings persist"
vm 'hey -s PowerMac set Running of Window 0 to false'
check "Running set false" 0 "$(get Running)"
shot powered-off
check "black screen when powered off" ok "$(python3 -I -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print("ok" if im.getextrema() == ((0, 0), (0, 0), (0, 0)) else "not black")
' "$OUT/powered-off.png" 2>&1)"
quit_app
check "settings saved" ok "$(vm 'test -s ~/config/settings/PowerMac/settings \
	&& echo ok')"
check "NVRAM saved" ok "$(vm 'test -s \
	~/config/settings/PowerMac/Profiles/Default/nvram.bin && echo ok')"
start_app
if [ -f "$MACOS_CD" ]; then
	check "Cd kept after restart" $TEST_DIR/macos.iso "$(get Cd)"
	vm "hey -s PowerMac set Cd of Window 0 to '\"\"'"
	check "Cd ejected" "" "$(get Cd)"
fi

echo "== profiles"
quit_app
start_app --profile Second
check "--profile makes a profile" Second "$(get Profile)"
check "two profiles" 2 "$(vm "hey PowerMac get Profiles of Window 0" \
	| grep -c '"result"')"
vm "hey -s PowerMac set Profile of Window 0 to Default"
check "switched to Default" Default "$(get Profile)"
check "switching to a missing profile fails" ok "$(vm "hey PowerMac set \
	Profile of Window 0 to Missing" | grep -q 'error' && echo ok)"
quit_app

echo "$PASSED passed, $FAILED failed (screenshots in $OUT)"
[ $FAILED -eq 0 ]
