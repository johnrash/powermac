/*
 * PowerMac emulator - machine configuration.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_MACHINE_CONFIG_H
#define PM_MACHINE_CONFIG_H

#include "../Types.h"

#include <string>

namespace powermac {

struct MachineConfig {
	// Power Macintosh 9500 models: processor(s), processor clock and bus
	// clock (ModelInfo()).
	enum Model {
		k9500_120,		// PowerPC 604 at 120 MHz, 40 MHz bus
		k9500_132,		// PowerPC 604 at 132 MHz, 44 MHz bus
		k9500_150,		// PowerPC 604e at 150 MHz, 50 MHz bus
		k9500_180MP,	// two PowerPC 604e at 180 MHz, 45 MHz bus
		k9500_200,		// PowerPC 604e at 200 MHz, 50 MHz bus
		kModelCount
	};

	struct ModelInfo {
		const char*	name;			// "9500/180MP"
		bool		is604e;
		int			cpus;
		u32			cpuHz;
		u32			busHz;
		u32			pvr;
		u32			hid1;			// 604e: PLL configuration
	};
	static const ModelInfo& Info(Model model);
	// The model with the name (as Info().name, or the part after "9500/"),
	// false if there is none.
	static bool ModelFromName(const char* name, Model& model);

	Model			model = k9500_180MP;
	u32				ramMB = 64;			// 16-1536, in memory banks
	int				cpuCount = 0;		// 0: the model's (1 for debugging)
	u32				pvr = 0;			// 0: the model's
	// Average CPU cycles per instruction, times 100.
	u32				cpiPercent = 100;
	// Emulated time slice per CPU, in microseconds.
	u32				quantumUs = 10;
	// Each processor on a host thread of its own (they run at the same
	// time between the time slice boundaries, where the devices run).
	// Faster when both are busy; runs are no longer reproducible.
	bool			threadPerCpu = false;

	std::string		romPath;			// the 4 MB "TNT" ROM
	std::string		nvramPath;			// persistent NVRAM (8 KB)
	std::string		pramPath;			// persistent Cuda PRAM and clock

	// The real time clock: 0 follows the host's clock; otherwise the
	// Macintosh time (seconds since 1904, local time) at power on.
	u32				fixedTime = 0;

	// Video: an IMS Twin Turbo 128 in PCI slot A1, with the expansion ROM
	// dumped from a real card (which carries its Open Firmware code and
	// Mac OS driver). No card without a ROM.
	std::string		cardRomPath;
	u32				vramMB = 8;			// 2, 4 or 8
	// Monitor sense code (the card's sense lines; the driver offers the
	// monitor's modes): see ImsTwinTurbo128.h.
	int				monitor = 0;

	// Storage: image paths ("" = no device); read only flags.
	struct ScsiTarget {
		enum Kind {
			kNone,
			kDisk,
			kCdRom,
			// a host folder (image: the folder); the front end builds a
			// volume of it and runs the machine with the volume's image as
			// a disk: BFS for BeOS (SharedFolder), HFS for Mac OS
			// (SharedMacFolder)
			kSharedBe,
			kSharedMac
		};
		Kind		kind = kNone;
		std::string	image;
		bool		readOnly = false;
	};
	// The internal bus (MESH: the hard disks) and the external one
	// (53C94: also the internal CD-ROM drive, ID 3). The controllers are
	// ID 7.
	ScsiTarget		scsiInternal[7];
	ScsiTarget		scsiExternal[7];

	// Ethernet (MACE): the address in the ROM, as on the wire.
	bool			ethernet = false;
	u8				ethernetMac[6] = { 0x00, 0x05, 0x02, 0x95, 0x00, 0x01 };

	std::string		floppyImage;		// the 3.5" SuperDrive
	bool			floppyWriteProtected = false;
};

}	// namespace powermac

#endif	// PM_MACHINE_CONFIG_H
