/*
 * PowerMac emulator - Bandit, Apple's PCI host bridge.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * The 9500 has two: the first at 0xF2000000 (Grand Central and slots
 * A1-C1 behind it), the second at 0xF4000000 (slots D2-F2). Each bridge
 * decodes 16 MB:
 *
 *   base + 0x000000  PCI I/O space (8 MB)
 *   base + 0x800000  configuration address
 *   base + 0xC00000  configuration data (+ register & 3)
 *
 * Configuration addresses are little endian (the ROM writes them with
 * stwbrx). Type 0 cycles select the device by a one-hot IDSEL line:
 * (1 << device) | (function << 8) | register, devices 11-31; type 1
 * cycles (bit 0) go to other buses, which there are none of. The bridge
 * itself is device 11 (106B:0001).
 *
 * The address register keeps the four bytes as written: the dual
 * processor card uses it as the secondary processor's start address
 * mailbox (written with stw, read with lwz).
 *
 * PCI memory is at the same addresses on both sides of the bridge, so
 * the devices map their memory BARs on the processor bus directly; bus
 * masters reach the RAM at its physical addresses.
 */
#ifndef PM_BANDIT_H
#define PM_BANDIT_H

#include "IoSpace.h"
#include "PciBus.h"
#include "PhysBus.h"
#include "../Types.h"

namespace powermac {

class Bandit : public MmioDevice, public PciDevice {
public:
	static const u32 kWindowSize = 0x1000000;
	static const u32 kIoSize = 0x800000;
	static const int kSelfDevice = 11;

								Bandit(PhysBus& bus, u32 base, int number);
	virtual						~Bandit();

			PciBus&				Pci() { return fPci; }
			IoSpace&			Io() { return fIo; }
			u32					Base() const { return fBase; }

			void				Reset();

	// MmioDevice
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return fName; }

	// PciDevice (the bridge's own configuration space)
	virtual	const char*			PciName() const { return fName; }

private:
			bool				_Target(u32 dataOffset, int& device,
									int& function, u32& reg) const;

			PhysBus&			fBus;
			u32					fBase;
			char				fName[16];
			PciBus				fPci;
			IoSpace				fIo;
			u8					fAddress[4];	// processor byte order
};

}	// namespace powermac

#endif	// PM_BANDIT_H
