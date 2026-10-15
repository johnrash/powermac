# PowerMac

PowerMac is a Haiku application that emulates Apple's Power Macintosh 9500,
including the dual processor 9500/180MP. It emulates the hardware itself,
so it runs the Mac's own ROM, Open Firmware, and the operating systems that
take over the hardware with their own drivers. It runs Mac OS 7.6.1 and
BeOS R5 for the Macintosh, on both processors of the 9500/180MP.

| Model | Processors | Bus |
|---|---|---|
| 9500/120 | PowerPC 604 at 120 MHz | 40 MHz |
| 9500/132 | PowerPC 604 at 132 MHz | 44 MHz |
| 9500/150 | PowerPC 604e at 150 MHz | 50 MHz |
| 9500/180MP (the default) | two PowerPC 604e at 180 MHz | 45 MHz |
| 9500/200 | PowerPC 604e at 200 MHz | 50 MHz |

## What is emulated

- **Processors**: a PowerPC 604/604e interpreter with the 604's hardware
  page table search. The second processor of the 9500/180MP is parked in
  the ROM until a system starts it, and then runs on a host core of its
  own (Preferences > Advanced). BeOS uses both processors; Mac OS 7.6.1
  only starts the second one for applications written for its
  Multiprocessing API, so under Mac OS one host core is busy.
- **Logic board**: the Hammerhead memory controller (16 MB to 1.5 GB), two
  Bandit PCI bridges, and the Grand Central I/O controller (interrupts,
  DBDMA, NVRAM).
- **Cuda**: the ADB keyboard and mouse, the clock and PRAM, and power. Shut
  Down turns the machine off.
- **Storage**: two SCSI buses. The internal one (MESH) takes hard disks; the
  external one (53C94) has the CD-ROM drive at ID 3. Disks are raw or QCOW2
  images; CDs are ISO or BIN/CUE images. Drive Setup accepts the disks as
  Apple drives. A Haiku folder can be a disk too (see "Sharing files with
  Haiku").
- **Video**: an IMS Twin Turbo 128 in slot A1 with a TI TVP3030 RAMDAC and
  8 MB of video memory, up to 1024x768 in millions of colors. The card's
  own ROM supplies its Open Firmware code and Mac OS driver; BeOS's own
  driver also uses the card's drawing engine.
- **Sound**: AWACS, played through the Haiku media kit (the startup chime
  included).
- **Floppy**: the SuperDrive (SWIM3) with Mac 400 KB and 800 KB (GCR)
  and 720 KB and 1.44 MB (MFM) disk images, raw or Disk Copy 4.2: Media >
  Insert floppy, or the floppy icon in the status line. Raw images may be
  cut short (the zeros at the end left out). Mac OS initializes blank
  images (a file of 1474560 or 819200 zero bytes) and ejects disks
  itself.
  Compressed Disk Copy 6 images (NDIF) go in as locked disks, also when
  they lost their resource fork (the map of their chunks) on the way, as
  long as the disk can be put back together without it; PowerMac says so
  when it cannot.
- **Serial**: the modem and printer ports (Z85C30). The modem port is Open
  Firmware's console. A port can go to a console window, TCP, a host serial
  port or a file. AppleTalk's LocalTalk on the printer port sees an idle
  network.
- **Ethernet**: the built-in MACE on a private network behind the host
  (NAT, like QEMU's user networking). The Mac gets `10.0.2.15` by DHCP;
  host ports can be forwarded to it.

Not yet emulated:

- sound input (it records silence);
- copy protected floppies (the SWIM3's raw track reads).

## What you need

PowerMac does not include Apple's software or the video card's ROM. You
need images of your own:

- **The Mac ROM**: the 4 MB ROM of a Power Macintosh 7200, 7500, 8500 or
  9500 ("TNT", $077D.28F2 or $077D.28F1). Put it in
  `~/config/settings/PowerMac/ROMs/9500.rom`, or choose it in the
  preferences (ROM & NVRAM).
- **The video card ROM**: the 64 KB expansion ROM of an IMS Twin Turbo 128
  card. Put it in `~/config/settings/PowerMac/ROMs/TwinTurbo128.rom`.
  Without it there is no display.
- **A Mac OS CD image**, such as Mac OS 7.6.1.

## Installing Mac OS

1. **Create a disk.** Preferences > Storage > New disk image, for example
   2048 MB in QCOW2 format, on internal SCSI ID 0. Put the Mac OS CD image
   in the CD-ROM drive: Media > Insert CD, or click the CD icon in the
   status line.
2. **Start from the CD.** Power the machine on. With a new disk that has
   nothing on it yet, the Mac waits about 25 seconds for that disk before
   it starts from the CD.
3. **Initialize the disk.** Open the CD, then Utilities > Drive Setup.
   Select the disk and click Initialize. Custom Setup can split it, for
   example into one volume for Mac OS and one to give to BeOS later.
4. **Install.** Run "Install Mac OS" from the CD. Choose the disk in step
   3, then install in step 4.
5. **Restart from the disk.** Special > Shut Down. Eject the CD (Media >
   Eject CD) and power on again.

## Starting BeOS

BeOS for the Macintosh starts from Mac OS with the BeOS Launcher. These
steps were tested with BeOS R5 Pro:

1. **Start Mac OS** from your disk with the BeOS CD in the drive.
2. **Run the Launcher.** Open the CD, then Macintosh > BeOS Mac Tools >
   BeOS_Launcher.
3. **Put the CD back.** The Launcher ejects the CD as it starts BeOS's boot
   loader, which then shows its boot menu. Put the CD back with Media >
   Reinsert, choose "Rescan for bootable volumes" and press Return.
4. **Boot BeOS.** Choose the CD's BeOS volume and press Return. The BeOS
   installer starts. To install BeOS on a disk, give it a partition of its
   own, for example the second volume Drive Setup made.

## Sharing files with Haiku

A Haiku folder can be a disk for Mac OS or for BeOS. In Preferences >
Storage, pick a free SCSI ID, set its kind to **Shared Mac** (for Mac OS)
or **Shared Be** (for BeOS) and choose the folder.

- At power on, the folder's files go into a fresh volume: HFS for Mac OS,
  BFS for BeOS. The system sees a disk named after the folder.
- When the system shuts down, the machine powers off and the changes go
  back to the folder: new, changed and deleted files. If a file changed on
  both sides, Haiku's version stays and the system's is saved next to it as
  "NAME (PowerMac)".
- Changes come back only after a clean shut down. If the machine is
  powered off without one, the volume is kept and used again at the next
  power on; its changes come back after the next clean shut down.
- Unlike SheepShaver's shared Unix volume, the sharing is not live: files
  changed on the Haiku side while the machine runs show up at its next
  power on.

Shared Mac, in more detail:

- Mac OS mounts a disk only with a SCSI driver on it, and PowerMac does not
  include Apple's. It copies the driver from a disk Drive Setup initialized
  in the same profile (your Mac OS disk).
- HFS volumes hold up to 2 GB, and names have up to 31 characters with no
  difference between upper and lower case. On the Mac, longer names are
  shortened and names that differ only in case get "#1"; the Haiku files
  keep their own names.
- Resource forks and Finder information (type, creator, icon position) are
  kept in Haiku attributes (`MACOS:RFORK`, `MACOS:FINDER_INFO`, and the
  `MACOS:CREATOR` and `MACOS:HFS_FLAGS` SheepShaver uses on BeOS), so Mac
  applications and documents survive the trip. A file without them gets a
  type and creator from its MIME type or extension: text opens in
  SimpleText, pictures in PictureViewer.
- The Finder's own files on the volume (its desktop database, the Trash
  while it is empty) do not come back to the folder.

## Using it

- **Mouse**: click the screen to capture the mouse. The right Control key
  releases it (you can pick another key in the preferences).
- **Keyboard**: all other keys go to the Mac. Alt is Command and the
  Windows key is Option.
- **Startup keys**: Machine > Restart holding restarts with keys held
  through the start:
  - C starts up from the CD;
  - Shift turns extensions off;
  - Command-Option-P-R resets PRAM;
  - Command-Option-O-F gives Open Firmware's prompt on the modem port's
    console;
  - Command-Option-Shift-Delete skips the startup disk.
- **Power key**: Machine > Power key shows Mac OS's shut down dialog.
- **Network**: turn on Ethernet in the preferences (Network). In Mac OS,
  open the TCP/IP control panel, choose Ethernet and "Using DHCP Server".
  TCP/IP then loads when an application needs it; to have it load at
  startup, choose Edit > User Mode > Advanced, click Options and turn off
  "Load only when needed".
- **Profiles**: Machine > Profile keeps several setups (disks, model, ROMs)
  to switch between. Each has its own NVRAM and PRAM.
- **Scripting**: the window answers `hey`. For example,
  `hey PowerMac get Speed of Window 0`, or `hey PowerMac type of Window 0
  with text=...`. MachineWindow.h lists the properties and commands.

## Command line tools

- `powermac-headless` runs a machine without a window. It can take
  screenshots, type, click at screen positions (closed loop on the pointer)
  and trace or dump memory, for tests and debugging. It takes the ROMs
  from `--rom` and `--card-rom`, or from the environment variables
  `POWERMAC_ROM` and `POWERMAC_CARD_ROM`.
  `powermac-headless --help` lists the options.
- `powermac-img` creates and inspects disk images.

## Building

The emulator core is portable C++17. On Linux, `make -C tests/linux check`
builds it with the address and undefined behavior sanitizers and runs:

- the CPU tests, checked against QEMU as an oracle for the 603e and the 604;
- the device tests;
- the disk image tests.

On Haiku, `make` builds the application (it needs haiku_devel, zlib_devel
and libslirp_devel), and `package/build-package.sh` builds the package.

`design/hardware.md` describes the emulated hardware and what the ROM and
the systems depend on. `design/provenance.md` lists the code shared with
the BeBox emulator, which PowerMac was started from.

## License

MIT. See LICENSE.
