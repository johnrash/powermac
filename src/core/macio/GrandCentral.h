/*
 * PowerMac emulator - Grand Central, the 9500's I/O controller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * A PCI device (106B:0002) on the first Bandit, device 16, with a 128 KB
 * register window the ROM puts at 0xF3000000:
 *
 *   0x00000  interrupt controller (little endian): events 0x20, mask
 *            0x24, clear 0x28, levels 0x2C
 *   0x08000  DBDMA channels, 0x100 each: 0 SCSI0 (53C94), 1 floppy,
 *            2/3 Ethernet out/in, 4-7 SCC A out/in, B out/in, 8/9 sound
 *            out/in, 10 SCSI1 (MESH)
 *   0x10000  53C94 SCSI (external bus)    0x11000  MACE Ethernet
 *   0x12000  ESCC (compatible)            0x13000  ESCC (MacRISC)
 *   0x14000  AWACS sound                  0x15000  SWIM3 floppy
 *   0x16000  VIA (Cuda)                   0x18000  MESH SCSI (internal)
 *   0x19000  Ethernet address PROM (a byte every 16)
 *   0x1A000  board ID (I/O bus device 1)
 *   0x1D000  NVRAM address latch (address >> 5)
 *   0x1F000  NVRAM data (a byte every 16: address & 0x1F)
 *
 * The interrupt controller has 32 sources. Bit 31 of the mask selects
 * the mode the ROM sets: every change of a source latches an event (and
 * writing bit 31 to the clear register clears them all); without it only
 * rising edges do. The processor's interrupt is events & mask. (After
 * MAME's heathrow.cpp, BSD-3-Clause, R. Belmont.) Mac OS finds its
 * sources in the levels register, not in the events: the DMA channels'
 * interrupts (sources 0-10, pulses) stay set there until the clear
 * register names them.
 *
 * Devices on the window are MmioDevices that get offsets from their own
 * base.
 */
#ifndef PM_GRAND_CENTRAL_H
#define PM_GRAND_CENTRAL_H

#include "Dbdma.h"
#include "../bus/PciBus.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <functional>
#include <memory>
#include <vector>

namespace powermac {

class GrandCentral : public PciDevice, public MmioDevice {
public:
	static const u32 kWindowSize = 0x20000;
	static const u32 kNvramSize = 0x2000;
	static const int kDmaChannels = 11;

	// DBDMA channels
	enum {
		kDmaScsi0 = 0,
		kDmaFloppy = 1,
		kDmaEthernetTx = 2,
		kDmaEthernetRx = 3,
		kDmaSccATx = 4,
		kDmaSccARx = 5,
		kDmaSccBTx = 6,
		kDmaSccBRx = 7,
		kDmaAudioOut = 8,
		kDmaAudioIn = 9,
		kDmaScsi1 = 10
	};

	// interrupt sources
	enum {
		kIrqScsi0Dma = 0,
		kIrqFloppyDma = 1,
		kIrqEthernetTxDma = 2,
		kIrqEthernetRxDma = 3,
		kIrqSccATxDma = 4,
		kIrqSccARxDma = 5,
		kIrqSccBTxDma = 6,
		kIrqSccBRxDma = 7,
		kIrqAudioOutDma = 8,
		kIrqAudioInDma = 9,
		kIrqScsi1Dma = 10,
		kIrqScsi0 = 12,			// 53C94
		kIrqScsi1 = 13,			// MESH
		kIrqEthernet = 14,
		kIrqSccA = 15,
		kIrqSccB = 16,
		kIrqAudio = 17,
		kIrqVia = 18,
		kIrqFloppy = 19,
		kIrqNmi = 20,			// the programmer's switch
		kIrqSlotA1 = 23,		// PCI slot A1 (Open Firmware's
								// AAPL,interrupts)
		kIrqPrimaryIpi = 30		// the dual processor card
	};

								GrandCentral(PhysBus& bus);
	virtual						~GrandCentral();

			void				Reset();

			// Places a device at 'offset' (4 KB granular) in the window.
			void				AttachDevice(u32 offset, u32 size,
									MmioDevice* device);

			DbdmaChannel&		Dma(int channel) { return *fDma[channel]; }

			void				SetInterrupt(int source, bool asserted);
			void				SetCpuInterruptCallback(
									std::function<void(bool)> callback)
									{ fCpuInterrupt = callback; }

			u8*					Nvram() { return fNvram; }
			bool				NvramDirty() const { return fNvramDirty; }
			void				ClearNvramDirty() { fNvramDirty = false; }
			// The Ethernet address as on the wire.
			void				SetEthernetAddress(const u8 address[6]);
			// The dual processor card interrupts the first processor when
			// the Ethernet address PROM's first word is read (source 30).
			void				SetPrimaryIpiEnabled(bool enabled)
									{ fPrimaryIpi = enabled; }

	// PciDevice
	virtual	const char*			PciName() const { return "grand-central"; }

	// MmioDevice
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "grand-central"; }

protected:
	virtual	void				BarsChanged();

private:
	struct Slot {
		MmioDevice*			device;
		u32					base;		// offset of the device's start
	};

			u32					_OwnRead(u32 offset, int size);
			void				_OwnWrite(u32 offset, u32 value, int size);
			void				_UpdateCpuInterrupt();

			PhysBus&			fBus;
			u32					fMappedBase;	// 0: not mapped
			Slot				fSlots[kWindowSize >> 12];
			// stand-ins for the devices not emulated yet
			std::vector<std::unique_ptr<MmioDevice> > fStubs;
			std::unique_ptr<DbdmaChannel> fDma[kDmaChannels];
			std::function<void(bool)> fCpuInterrupt;
			bool				fCpuInterruptLine;

			u32					fEvents;
			u32					fMask;
			u32					fLevels;
			u32					fDmaLatched;	// DMA interrupts not cleared

			u8					fNvram[kNvramSize];
			u8					fNvramLatch;
			bool				fNvramDirty;
			u8					fEthernetProm[8];
			bool				fPrimaryIpi;
};

}	// namespace powermac

#endif	// PM_GRAND_CENTRAL_H
