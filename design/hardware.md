# The emulated hardware: Power Macintosh 9500/180MP

This is the reference for what PowerMac emulates and why it behaves as it
does. Register-level details live in the header comment of each device's
source file (listed below); this document gives the map, the boot path, and
the behaviors the ROM and the operating systems depend on, with where each
fact came from.

PowerMac is a low-level emulator. It runs Apple's real Old World ROM
("TNT", $077D.28F2), Open Firmware 1.0.5 from that ROM, the video card's own
FCode and Mac OS driver, and Mac OS or BeOS on top. Nothing is patched.

## Sources and licenses

PowerMac is MIT licensed. What it was built from:

| Kind | Sources | Use |
|---|---|---|
| Bytes | The ROM, the Twin Turbo 128 card ROM, Mac OS 7.6.1 and BeOS media | Disassembled with `tools/disasm.sh` (objdump), `tools/fcode-detok.py`, a PEF reader and a 68K disassembler |
| BSD-licensed code | MAME `apple/heathrow.cpp` (Grand Central's interrupt controller) | Adapted, with the notice kept in the source |
| Facts only | Linux (`mesh.h`, `imsttfb.c`, `adbhid.c`, `powermac/smp.c`), NetBSD's macppc drivers, DingusPPC, SheepShaver/BasiliskII, QEMU | Register names and bit meanings; no code copied (GPL) |
| Documents | Apple Technote 1062, the TVP3030, Z85C30, 53C94 and 6522 datasheets | Register behavior |

When a behavior below was found by reading the ROM or a driver, the
address is given so it can be checked again.

## Machine map

| Physical address | Contents | Source file |
|---|---|---|
| `0x00000000` | RAM, banked by Hammerhead (8 MB to 1 GB) | `board/Hammerhead` |
| `0x80000000`–`0xEFFFFFFF` | PCI memory (the Twin Turbo's BAR: `0x82000000` as the ROM assigns it) | `bus/PciBus` |
| `0xF2000000` | Bandit 1: PCI I/O, configuration at `+0x800000` / `+0xC00000` | `bus/Bandit` |
| `0xF3000000` | Grand Central (BAR assigned by the ROM) | `macio/GrandCentral` |
| `0xF4000000` | Bandit 2 | `bus/Bandit` |
| `0xF8000000` | Hammerhead | `board/Hammerhead` |
| `0xFFC00000` | 4 MB ROM; reset vector `0xFFF00100` | `board/PowerMacMachine` |

PCI devices:

| Bus | Device | What |
|---|---|---|
| Bandit 1 | 11 | the bridge (106B:0001) |
| Bandit 1 | 13 (slot A1) | IMS Twin Turbo 128 (10E0:9128), interrupt to Grand Central source 23 |
| Bandit 1 | 16 | Grand Central (106B:0002) |
| Bandit 2 | 11 | the bridge; slots D2–F2 empty |

Grand Central's interrupt sources:

| Source | Device |
|---|---|
| 0–10 | DBDMA channels (0 SCSI0, 1 floppy, 2/3 Ethernet, 4–7 SCC, 8/9 sound, 10 SCSI1) |
| 12 | 53C94 (external SCSI bus) |
| 13 | MESH (internal SCSI bus) |
| 15, 16 | ESCC channels |
| 18 | VIA (Cuda) |
| 20 | NMI button |
| 23 | PCI slot A1 (the video card) |
| 30 | the primary processor's IPI |

## Time

The machine counts 12 ticks per bus clock (540 MHz at a 45 MHz bus). A
604e at 4:1 takes 3 ticks per cycle, at 3:1 four; the time base advances
every 48 ticks (bus/4). Devices schedule `Timer`s in ticks
(`board/Scheduler`).

## Boot path and what each step needs

1. **ROM startup** (`0xFFF03000`): the 604e takes the ROM's 604 path. The
   ROM sizes memory through Hammerhead's 26 banks: it places bank *n* at
   *n* × 64 MB, writes the pattern `'KurtLiza'` at the top of the window and
   finds the size by aliasing. It then packs the banks and stores bases and
   sizes in NVRAM at `0x1048 + 8n`. Writes to the ROM must be ignored
   quietly; the ROM writes -16 to `0xFFFFFFF0` between probes.
2. **Bus speed**: the ROM times VIA timer 1 (783.36 kHz) against the
   decrementer (bus/4) and picks the bus speed from a table.
3. **Open Firmware 1.0.5**: the console is ttya, ESCC channel A. Holding
   Cmd-Opt-O-F (ADB codes `0x37 0x3A 0x1F 0x03`) at power-on gives the `ok`
   prompt. Serial input must be paced, at 3 ms per character, or OF's
   16-character type-ahead buffer drops characters. OF's own FCode uses
   `0x00` as a two-byte token prefix (`tools/fcode-detok.py --apple`).
4. **The video card's FCode** builds the `IMS,tt128mb8A` node and attaches
   the Mac OS driver as the property `driver,AAPL,MacOS,PowerPC`.
5. **The nanokernel and the 68K emulator** start Mac OS. The nanokernel
   sizes the caches by timing loads for PVRs outside its table (it has
   entries only for families 1, 3, 4 and 6). On a 604e (PVR 9) without a
   cache timing model, its second loop gets a step of 0 and hangs. The CPU
   therefore models a 4-way L1 data cache (16 KB on the 604, 32 KB on the
   604e) for real-mode loads.
6. **The Start Manager** (68K code at ROM `0x20F6`) looks for the default
   startup device, the SCSI ID in `$0C2F`, which is 0 with fresh PRAM. It
   waits up to the PRAM timeout for it; a timeout of 0 means 25 s. Only
   then does it fall back to other bootable devices such as the CD. A real
   machine waits the same way, so booting the CD with fresh PRAM costs 25 s.
   The ROM's search does not read the KeyMap: the C key is handled by the
   CD's driver.
7. **Mac OS** loads the display driver from the card and the SCSI drivers
   from the disks' driver partitions.

## Devices

### Hammerhead (`board/Hammerhead.h`)

Memory controller and the dual processor card's registers: ID, board type
(bit `0x40000000` of `+0x20` means a 9500), the 26 memory banks, the
arbiter (bit `0x02` of `+0x90` means a dual-CPU card, bit `0x04` is TBEN),
"who am I" (`+0xB0`), and the secondary processor's interrupt (`+0xC0`).

The second processor:

- It parks in the ROM at reset: `mtmsr` with EE|POW, then `b -4`. The CPU
  detects this loop as idle.
- An IPI sends it to the address in the Bandit 1 configuration address
  register at `0xF2800000`, which doubles as a mailbox.
- Open Firmware's probe of the second CPU would leave it in an exception
  loop: the probe stores with data translation on and no mapping. The real
  OF finds only one CPU (Technote 1062, Linux). So the card's bit reads 0
  until CPU0 enters the nanokernel (PC in `0xFFF10000`–`0xFFF1FFFF`).
- BeOS asserts and clears the secondary IPI a few instructions apart. A
  clear coming from the other CPU therefore waits until CPU1 has taken the
  interrupt (pulse stretching).
- The primary IPI is a byte read of `GC+0x19000`, which raises Grand
  Central source 30.

### Bandit (`bus/Bandit.h`)

Configuration cycles use a one-hot IDSEL, `(1 << dev) | (fn << 8) | reg`.
Data is little endian: the ROM accesses it with `lwbrx`/`stwbrx`.

### Grand Central (`macio/GrandCentral.h`)

The interrupt controller (events `0x20`, mask `0x24`, clear `0x28`, levels
`0x2C`) follows MAME's `heathrow.cpp`. Bit 31 of the mask selects the mode
the ROM uses, and writing `0x80000000` to the clear register clears every
event. Grand Central also holds the eleven DBDMA channels, the board ID, the
NVRAM (8 KB; XPRAM at `0x1300`, Open Firmware's configuration at `0x1800`)
and the Ethernet address PROM. Sub-devices that are not emulated yet answer
through logging stubs, so their accesses show in `--log bus=info`.

Mac OS never reads the events register. Its external interrupt handler
(nanokernel code in RAM at `0x8C9E0`) writes `0x80000000` to the clear
register, reads the mask and the levels, and picks the 68K interrupt level
from `levels & mask`:

| Sources | Level |
|---|---|
| 20 (NMI) | 7 |
| 0–10 (DMA), 15–16 (SCC) | 4 |
| 14 (MACE) | 3 |
| 12–13 (SCSI) and the slots | 2 |
| 18 (VIA) | 1 |

The handlers it calls also find their sources in the levels. A DBDMA
interrupt is a pulse, so Grand Central keeps the DMA sources (0–10) set in
the levels register until the clear register names their bit; the ROM's
DMA interrupt handlers do that (`0x08` for the Ethernet receive channel).
Before this, no DMA interrupt reached a Mac OS driver: Ethernet, sound
output and serial DMA all depend on them.

The Ethernet PROM (`0x19000`, a byte every 16) holds the address and a
checksum. Open Firmware reverses the bits of every byte when the first one
is `0x10` (Apple's `08` bit reversed), so `08:00:07` addresses are stored
bit reversed and any other address as it is; "local-mac-address" then
shows the configured address.

### DBDMA (`macio/Dbdma.h`)

Each descriptor is 16 bytes, little endian. The commands are OUTPUT/INPUT
MORE/LAST, STORE_QUAD/LOAD_QUAD, NOP and STOP. Each descriptor carries
interrupt, branch and wait conditions, tested against the S7–S0 status bits.
Data moves at the device's pace: `Kick()` resumes a transfer, and
`EndInput()` ends a short read. A device learns that an OUTPUT_LAST
descriptor has ended (the end of an Ethernet frame) from `DmaOutputLast()`.

A channel that reaches a STOP stays idle, still RUN, until software sets
WAKE or starts RUN again; only then does it fetch the command at the
command pointer. A device's kick must not wake it. The startup chime ends
at a STOP with RUN still set; Mac OS's sound driver then writes its own
command pointer and waits for the channel to be idle before it starts its
ring. When AWACS's kicks started that ring early, the channel never went
idle again and Mac OS froze at "Starting Up...".

STORE_QUAD and LOAD_QUAD (key 6) move 1, 2 or 4 bytes between the
descriptor's command-dependent field and any physical address, device
registers included. Apple's Ethernet driver ends each transmitted frame
with LOAD_QUADs of the MACE's XMTRC, XMTFS and IR.

### VIA and Cuda (`macio/Via6522.h`, `macio/Cuda.h`)

Cuda is emulated at the packet level behind the VIA's shift register and
port B handshake (TIP, BYTEACK, TREQ). Every step raises the shift register
interrupt after a real Cuda's delay of 61, 71, 88 or 30 µs. It provides:

- ADB: an Apple Extended Keyboard and a mouse;
- the 1904-epoch clock and 256 bytes of PRAM;
- autopoll;
- one-second packets;
- power down and reset, so Shut Down turns the machine off.

**One-second packets are Mac OS's clock.** Mac OS 7.6.1 never asks Cuda
for the time: it sets one-second mode 3 (pseudo command `0x1B`) and keeps
the low-memory global Time (`0x20C`) from the packets, its handler at ROM
`0xFFC0DA28` counting the ticks. Each time Cuda's seconds change it sends
the GET_TIME reply with the time (`01 00 03` and four bytes) first, after a
second that had to wait for the host, and every fourth second, and the
one-byte tick packet (`03`) between (mode 2: the reply without the time;
mode 1: always the time). These are facts from DingusPPC's account of
Apple's Cuda ERS. Without them Time stayed at `0xFFFFFFFF` (February 2040):
files and volumes got that date, the clock in the menu bar stood still,
and with virtual memory on Apple System Profiler and the Word 5.1
Installer hung at launch.

**A synchronization turns Cuda's own packets off.** At the end of the
synchronization handshake (TIP high with BYTEACK low, then both high)
autopoll and the one-second packets stop until asked for again (same
source). BeOS's driver syncs when it starts and never asks for one-second
packets: while Mac OS's mode 3 went on, BeOS took the unasked packets for
replies, and its mouse got slower, its clock stopped, Pulse did not come
up, and in the end it hung with both processors busy.

The ADB keyboard reports held keys again after a SendReset or Flush. The ROM
resets the ADB about 50 times during start-up, and without this keys held
at power-on (boot keys) would be lost. Raw ADB Control is `0x36`; `0x3B` is
the left arrow (Linux `adbhid.c`). `host/KeyCodes` maps Haiku keys and
ASCII to ADB codes.

### ESCC (`macio/Escc.h`)

A Z85C30, decoded at both the compatible (`0x12000`) and MacRISC
(`0x13000`) addresses. Asynchronous mode goes to the host serial backends.

The synchronous modes model an idle LocalTalk network. AppleTalk opens
LocalTalk on the printer port at startup by default: SDLC with FM0, the
DPLL, and the receiver hunting with address search.

- **Waiting for a quiet line.** The driver polls RR0 until the line is
  quiet: Sync/Hunt (`0x10`) set means no flag has been seen.
- **Ending a frame.** It sends its frames and waits for each to end with
  the Tx underrun/EOM latch (RR0 `0x40`), which a reset also sets and the
  `0xC0` command in WR0 clears.
- **The hang.** With Sync/Hunt reading clear, the installed system hung at
  "Starting Up…".
- **Interrupts.** External/status interrupts follow changes of the RR0
  bits that WR15 enables.

### SCSI: MESH (`macio/Mesh.h`) and 53C94 (`macio/Ncr53c94.h`)

The internal bus is MESH (DBDMA channel 10, interrupt 13); the external bus
is the 53C94 (channel 0, interrupt 12). Targets are `pci/ScsiDevice`.

The MESH behaviors the drivers depend on were found by tracing the ROM's
MESH SIM (native code at `0xFFEB7xxx`) and the RAM driver Drive Setup uses:

- **PIO transfers.** A transfer sequence command without DMA starts first.
  The driver then fills or drains the FIFO, and "command done" comes when
  the count reaches 0. The ROM sends CDBs this way.
- **ACK on the last byte.** When a transfer's count runs out, MESH keeps ACK
  asserted on the last byte until the next sequence command, so the target
  cannot raise REQ for its next phase before then. Bus status 0 shows the
  new phase lines with ACK and no REQ.
  - After the status byte, the RAM driver spins until REQ drops, then
    issues message-in.
  - After the command phase, the ROM issues sequence `0x09` (wait for bus
    free). That releases ACK. The target then asserts REQ for its data
    phase, and the wait ends with a **phase mismatch** exception. On a
    target that does leave the bus, it ends with "command done" instead
    (ROM `0xFFEBB478`).
- **Clearing.** Writing the interrupt register's exception or error bit
  clears the exception or error register, as Linux's driver relies on.
- **Reset.** Sequence `0x0E` (reset) ends with "command done". BeOS's
  driver waits for it.
- **Message out.** Message out is FIFO or DMA driven like the other
  transfers. BeOS selects with ATN, issues message out, and only then
  writes IDENTIFY into the FIFO. If that byte is left behind, it becomes
  the first CDB byte (INQUIRY reached the disk as opcode `0x80`).

The 53C94 serves the CD on the external bus through DBDMA. Its 24-bit
transfer count uses register 14 as the high byte.

**Drive Setup** (1.1, on the 7.6.1 CD) lists a disk as "not supported"
unless MODE SENSE page `0x30` answers "APPLE COMPUTER, INC". It asks for 33
bytes: a 4-byte header, an 8-byte block descriptor, 2 bytes of page header
and the 19 characters of the name. The Apple CD-ROM extension makes the
same check, and without it no CD that lacks its own driver mounts (the
BeOS CDs). Apple-firmware drives answer this page, so `ScsiDisk` and
`ScsiCd` both do. Disks identify as "QUANTUM FIREBALL1080S" and the
CD as "SONY CD-ROM CDU-8003", which Apple's CD driver accepts.

### Floppy: SWIM3 (`macio/Swim3.h`)

The SWIM3 at `0x15000` (DBDMA channel 1, interrupt 19) is emulated with a
SuperDrive, from the register layout in Linux's `swim3.c`, with the drive
behavior of DingusPPC's and MAME's (facts):

- **Registers.** One every 16 bytes. Writes to the control register
  clear its bits; writes at the status offset set them.
- **Sense lines.** SEL and CA0-2 choose one of 16 drive sense lines. The
  status register shows the line as on the cable, low when active, in
  both bit 3 (Linux's DATA) and bit 2. The Mac OS ROM reads bit 2: its
  drive type routine at `0xFFCFA1B8` builds an index from lines 15, 7, 6
  and 5 (index 11, type 4, is a SuperDrive). Line 3, the eject latch, is
  the exception: it is high after a disk is inserted or ejected until
  action 12 clears it. With it the other way round, Mac OS takes the disk
  for another one, ejects it and asks for it back.
- **Two drive selects.** Only drive 1 (control `0x02`) answers. Mac OS
  polls both drives for a disk, and a drive 2 that mirrored drive 1
  mounted every disk twice.
- **Actions.** A rising LSTRB performs the action those lines select:
  seek direction, step, motor on and off, eject, clear the eject latch
  (12), and the drive's MFM or GCR mode (9, 13; line 13 reports it).
- **The disk turns.** The sector under the head follows the emulated
  time (300 rpm for MFM; for GCR 394 to 590 rpm in five zones of 16
  cylinders with 12 down to 8 sectors, numbered from 0 two slots apart:
  0, 6, 1, 7, ...). A scan reports the sectors as they come, and reads and
  writes wait for theirs. Mac OS tells disk formats apart by timing scans
  and by the format byte of the last sector header, which register `0x0C`
  returns when read (2 for MFM; the gap 3 length when written; the table
  at `0xD2362` in its RAM driver).
- **The track and sector seen.** The current track register has the head
  in bit 7; the ROM checks it.
- **Writes and formatting.** The DMA stream carries what the controller
  writes: gap, sync, marks escaped with `0x99` (`99 A1` three times,
  `99 FB`, `99 0F`), the 512 data bytes as they are, then `99 04` for the
  CRC. FORMAT with ACTION writes a track for one revolution; the sectors
  its ID fields name get the bytes of the data fields that follow.
- **GCR.** Facts from MAME's swim3.cpp (BSD-3-Clause) and Mac OS's
  driver: with the controller in GCR mode (setup `0x04`; Mac OS writes
  `0x4C`) the address fields give the track (head in bit 7), the sector
  and the format byte (`0x22` two sides, `0x02` one). The sector asked
  for ("first sector"; `0x40`: any) hands the DMA the 704 nibbles after
  the data mark as 6-bit values (without conversion, setup `0x10`, as
  disk bytes): the sector number, 699 for the 12 tag and 512 data bytes,
  4 of checksum. Mac OS's driver decodes them (ROM `0xFFCF97EA`: three
  bytes from four nibbles, a running checksum of three bytes) into a
  724-byte slot of its track cache. Writes come the same way after sync
  bytes and `D5 AA AD`. In what Mac OS writes, bytes below `0x40` are
  values the controller encodes (it gives `96` and `AD` so, as `00` and
  `0B`, and `DE` as `27`) and the others disk bytes with the top bit
  clear (the self-sync groups `3F BF 1E 34 3C 3F`) or as they are (`D5`,
  `AA`). Its formatted tracks are longer than a revolution (a long gap
  first, which the end writes over); after a format it scans for the
  first header and wants sector 0, else it formats again with other gaps
  (five times, then it gives up): the format runs from the index to just
  before the next one but one. A GCR disk shows no sectors in MFM mode
  and an MFM one none in GCR mode, which is how Mac OS tells them apart.
- **Commands.** DO_SEEK and DO_ACTION complete with interrupts. Sector
  data moves by DMA.
- **Disks** (`storage/FloppyImage.h`). 400 KB, 800 KB, 720 KB and 1.44 MB
  images, raw (also cut short: the HFS, MFS or FAT volume gives the size)
  or Disk Copy 4.2 (84-byte header, then the data and 12 tag bytes a
  sector; its checksums are updated after writes). Mac OS 7.6.1 mounts
  all four, initializes blank ones ("Macintosh 1.4 MB"), and ejects them.
  Compressed Disk Copy 6 images (NDIF) whose resource fork (the chunk
  map) is lost are put back together when there is one way to: the data
  fork holds the sectors before the allocation blocks as they are, the
  used blocks (one run from block 0 on Disk Copy's disks; free blocks are
  left out) in chunks of 512 sectors, each ADC compressed or stored, and
  the next to last sector as it is. ADC chunks are independent (no copy
  reaches before its chunk), which is how the chunk size was found; the
  Word 5.1a images came back identical, file by file, to raw copies of the
  same disks. Such a disk goes in locked; others are refused with a
  message.

Before SWIM3 existed, its stub read back zeros, which tells drivers a
disk is in. BeOS's loader then waited forever for a sector.

### Sound: AWACS (`macio/Awacs.h`)

AWACS at `0x14000` plays DBDMA channel 8 at the selected rate (bits 10-8
of the control register: 44100 down to 7350 Hz) into the host's
`AudioSink`. Samples are 16-bit stereo, big endian. Codec register 4 is
the speaker's attenuation and register 1 its mute. The ROM plays the
startup chime at 22050 Hz with both channels 14 steps down.

### Ethernet: MACE (`macio/Mace.h`)

The Am79C940 at `0x11000` (registers every 16 bytes; DBDMA channels 2 and
3; interrupt 14) connects to the host through the user mode network
(slirp). The registers follow the Am79C940 data sheet and NetBSD's
`am79c950reg.h`. The chip ID is `0x0940`.

- **Transmit.** The DMA fills a frame; the end of an OUTPUT_LAST
  descriptor sends it. After its time on a 10 Mbit/s wire, XMTFS becomes
  valid, XMTINT is raised and the transmit channel's S5 is set. Apple's
  driver waits for S5 after each frame (wait select `0x00200020`). The chip
  takes no more data until XMTFS is read.
- **Receive.** A frame for this station (the address, broadcast, the
  multicast hash in LADRF, or promiscuous) goes to the DMA followed by its
  FCS and four status bytes: RCVCNT, RCVSTS (status bits 7–4, count bits
  11–8), RNTPC and RCVCC. The FCS is left out of 802.3 frames when
  ASTRPRCV strips them. The end of the frame ends the descriptor (Apple's
  driver uses INPUT_MORE) and sets the receive channel's S6, which the
  drivers look for in the written-back status. Frames wait in the chip
  while the DMA is stopped, up to eight of them.
- **Loopback.** UTR's loopback bits send a frame back to the receiver
  instead of the network. Apple's driver ("Ethernet (Built-In)", the Grand
  Central/MACE DLPI driver) tests internal and then external loopback
  before it opens; a failed test leaves the chip off.

Open Firmware 1.0.5's `boot enet:` sends its BOOTP request to IP 0.0.0.0
from UDP port 1234. Slirp does not answer it, so netbooting from Open
Firmware does not work on the user mode network.

### Video: IMS Twin Turbo 128 and TVP3030 (`video/ImsTwinTurbo128.h`, `video/Tvp3030.h`)

The card is emulated at the chip level. Its 64 KB ROM, dumped from a real
card, supplies the Open Firmware FCode and the Mac OS driver
`.Display_TwinTurbo8A`. BAR 0 (32 MB) holds the VRAM at 0, the "cosmo"
registers at `0x800000` (32-bit, little endian) and the RAMDAC at
`0x840000`.

- **Geometry.** Width = (HSB − HEB) × 16 pixels at any depth; height =
  VSB − VEB. Pitch and start are SPR and SSR in address units: 8 bytes
  with both memory banks interleaved (SCR bit `0x10000`, as the Mac OS
  driver sets it), 4 bytes otherwise (BeOS's driver).
- **Drawing engine.** Used by BeOS's driver. It fills and copies
  rectangles. S1SA is the source, SP the source pitch (high half) and
  step (low half), DSA the destination, CNT (rows − 1) << 16 | (bytes −
  1), DP_OCTL the destination step, CLR the color. BLTCTL starts the
  operation: `0x200000` fills, `0x05` copies, bit `0x80` copies right to
  left. It completes at once, so the busy bits (SSTATUS `0xC0`) never
  show. A fill stores CLR little endian: BeOS's IXMicro add-on swaps 16-
  and 32-bit colors (`__swap_int32`) before it loads CLR. Mac OS does not
  use the engine.
- **Byte order.** Both systems write big endian pixels and set SCR bits
  9–8 for their depth: 3 for 32 bits (Mac OS `0x153DC`, BeOS `0xD398`), 1
  for 16 bits, 0 for 8. The display then reads 32-bit pixels, or 16-bit
  ones, with their bytes swapped (Linux's imsttfb calls the field
  "byteswap"). BeOS's add-on reports "argb" to the app_server on a Mac
  ("bgra" on a BeBox, from the machine ID in `get_system_info`). When the
  fills were stored big endian, BeOS's desktop and window backgrounds
  came out in the wrong colors at 15 and 32 bits.
- **Pixel clock.** 8 × 14.31818 MHz × (65 − M) / (65 − N) / 2^P.
- **Monitor sense.** The sense lines are GIO bits 3–5. The monitor answers
  as an Apple monitor would; the default is an Apple 19" at 1024x768, which
  is what lets the driver offer that mode.
- **Vertical blank interrupt.** SSTATUS (`0x90`) bit `0x10` is set at each
  vertical blank, and writing it with that bit set clears it. STGCTL
  (`0x60`) bit `0x400` enables the interrupt.
  - The Mac OS driver queues CLUT and gamma changes and applies them only
    in its VBL handler (driver code at `+0x7F30`; the enabler at `+0x802C`
    sets the bit, the disabler clears it).
  - Before drawing, the driver fills the CLUT with gray `0x9E9E9E`. With no
    VBL interrupt that gray is never replaced: Mac OS draws the desktop,
    but in 32-bit direct color the screen stays solid gray.
- **Where the driver is.** It is stored in the FCode as 252-byte `b(")`
  strings joined by `encode-bytes`/`encode+`; the separators are
  `01 15 [01 12] 12 LL`. Removing them gives the PEF container, which
  starts at ROM offset `0x1B32`. Its TOC is the data section's base.

## BeOS for the Macintosh

BeOS R5 boots on the emulated 9500/180MP, both processors running, as on
a real machine:

1. **Mac OS.** Mac OS 7.6.1 starts from the installed disk with the R5 CD
   in the drive. The CD mounts as "BeOS_Tools" (ISO 9660).
2. **The Launcher.** BeOS_Launcher, in Macintosh/BeOS Mac Tools on the
   CD, reads the boot loader and kernel from the CD's BFS track 3 itself,
   switches the screen to 8 bits, ejects the CD (START STOP UNIT with LoEj)
   and starts the "Be Boot Loader".
3. **The boot loader.** It uses its own MESH, 53C94, SWIM3 and Cuda
   drivers, polling with interrupts masked, and finds no bootable volume
   because the CD is out. It shows its boot menu.
4. **Booting BeOS.** Put the CD back (Media > Reinsert in the app,
   `--insert-cd` in headless) and press Return on "Rescan for bootable
   volumes". The CD's BFS volume ("BeOS 5 Pro Edition") is listed; Return
   boots it.
5. **The desktop.** The kernel starts the second processor through the
   mailbox and IPI, and BeOS's own Twin Turbo driver sets 640x480 in 8
   bits. The installer appears.

The loader's 8-bit screen stays gray. Mac OS's driver queues CLUT changes
for its VBL interrupt handler, and the Launcher sets the palette and hands
over to BeOS within one frame, so the handler never runs again. Whether a
real machine wins this race is not known.

## Verified so far

- The ROM reaches Open Firmware's `ok` prompt. The device tree lists both
  Bandits, Grand Central and its children, Hammerhead and the IMS card.
- With no disk, the flashing question-mark disk appears after the 25 s
  wait.
- Mac OS 7.6.1 boots from the CD image (external ID 3) to the Finder at
  1024x768 in millions of colors.
- Drive Setup initializes a qcow2 disk on the internal bus (two HFS
  volumes), and the installer installs Mac OS 7.6.1 onto it.
- Special > Shut Down powers the machine off through Cuda.
- The installed disk boots to the Finder; Gestalt reports machine type 67
  (the 9500).
- BeOS R5 boots from its CD to the installer, with both processors, and
  installs to a BFS volume made by its installer; the installed system
  starts through the Launcher and shows the right colors at 8, 15 and 32
  bits per pixel.
- The ROM's startup chime plays through AWACS, and so do Mac OS's alert
  sounds (SimpleSound).
- Mac OS 7.6.1's TCP/IP gets `10.0.2.15` from the user mode network's DHCP
  server through the MACE (Ethernet, DHCP, "Load only when needed" off).
- Open Firmware sends BOOTP requests through the MACE.
- Mac OS mounts 1.44 MB, 800 KB and 400 KB floppy images (raw and Disk
  Copy 4.2), writes to them, initializes blank ones (1.44 MB and 800 KB)
  and ejects them; the app sees its ejects.
- `tests/linux/device-tests` covers the behaviors above under ASan and
  UBSan in `make -C tests/linux check`:
  - MESH PIO, the ACK hold, reset, message out and DMA;
  - SCSI page `0x30`;
  - the Twin Turbo VBL interrupt, drawing engine and byte order;
  - SWIM3 sense lines (both status bits, the eject latch, drive 2),
    seeks, the turning disk, DMA reads, writes and formatting; GCR reads
    (decoded as the ROM does), the interleave, writes, formatting as Mac
    OS writes it; floppy image formats and Disk Copy checksums;
  - AWACS playback;
  - ESCC SDLC;
  - MACE transmit, receive, the address filter, loopback, S5/S6 and
    LOAD_QUAD;
  - Grand Central keeping DMA interrupts in the levels;
  - ADB held keys and key codes;
  - Cuda's handshake (the host side as the ROM drives it), GET_TIME,
    the one-second packets and the synchronization stopping them.

## Known gaps

- Holding C does not yet shortcut the startup wait for the CD.
- MACE's programmed I/O (the FIFO registers without DMA) is not emulated;
  the drivers seen so far use DMA.
- BeOS R5 reads `0xF8800064` about a hundred times at startup (unclaimed;
  not yet known what it probes).
- The drawing engine only fills and copies. Mono expansion and XOR raster
  operations are logged as not emulated when used.
- The SWIM3's copy protect mode (raw track reads, setup `0x02`) is not
  emulated.
- Sound input gives silence.
