/*
 * PowerMac emulator - Hammerhead, the 9500's memory controller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Registers at 0xF8000000, one every 16 bytes (32 bits, big endian), as
 * the TNT ROM's startup code and Open Firmware use them:
 *
 *   0x000  ID: 0x39 in the top byte (a Hammerhead; Platinum is 0x30)
 *   0x020  board: bit 0x40000000 = second PCI bridge (a 9500)
 *   0x030  bus timing configuration
 *   0x050-0x080  memory timing (written from a table the ROM picks by
 *          0x030)
 *   0x090  arbiter: byte bit 0x02 = a dual processor card, bit 0x04 =
 *          the processors' time base enable (TBEN; BeOS clears it to
 *          synchronize their time bases)
 *   0x0b0  who am I: bit 0x08000000 when the secondary processor reads
 *   0x0c0  secondary processor interrupt (byte): 0 asserts it, bit 7
 *          set clears it
 *   0x0e0  L2 cache: byte bit 0x80 = present (none here)
 *   0x1c0 + 0x20 * n, 0x1d0 + 0x20 * n  memory bank n (0-25): the
 *          bank's base address (bits 29-22 in the top byte of the
 *          second register, bit 30 in bit 24 of the first one); bit
 *          0x04000000 of the first one interleaves banks n and n + 1
 *
 * Memory banks: the ROM sizes each bank by placing bank n at n * 64 MB
 * and writing a pattern at the top of that window, which a smaller bank
 * mirrors further down (its address lines end there); then it packs the
 * banks from 0 up and stores their bases and sizes in the NVRAM. A bank
 * here answers from its base up to the next higher bank base (at most 64
 * MB, the window), repeating its memory.
 *
 * The dual processor card: Open Firmware 1.0.5 probes the second
 * processor by its interrupt with a routine that stores with data
 * translation on and no mapping for it (the ROM's handler turns it on),
 * which would leave that processor in an exception loop. On the real
 * machine Open Firmware knows nothing of the second processor (Linux's
 * notes; Apple's Technote 1062) and the systems find it parked in the
 * ROM, the interrupt still pending (BeOS clears it before it starts
 * the processor). How is not known: here the card's bit reads 0 until
 * the ROM leaves Open Firmware (RevealDualProcessor()), so the probe is
 * skipped.
 *
 * The secondary processor's interrupt: a processor running in a later
 * time slice must not miss a short pulse (BeOS asserts and clears it a
 * few instructions apart; a real 604 spinning in the ROM's loop takes it
 * at once): a clear by another processor waits until the secondary has
 * taken the interrupt (SecondaryInterruptTaken()).
 */
#ifndef PM_HAMMERHEAD_H
#define PM_HAMMERHEAD_H

#include "../bus/PhysBus.h"
#include "../Types.h"

#include <functional>
#include <vector>

namespace powermac {

class Hammerhead : public MmioDevice {
public:
	static const u32 kBase = 0xf8000000;
	static const u32 kSize = 0x800;
	static const int kBanks = 26;
	static const u32 kBankWindow = 64 << 20;

								Hammerhead(PhysBus& bus, bool dualCpu);
	virtual						~Hammerhead();

			// The RAM, bank after bank (sizes in bytes, 0: empty; powers
			// of two up to 64 MB).
			void				SetMemory(u8* ram,
									const std::vector<u32>& bankSizes);
			// Splits 'megabytes' of RAM into banks the way DIMM pairs
			// fill them (largest first, pairs of equal banks).
	static	std::vector<u32>	BankSizesFor(u32 megabytes);

			void				Reset();

			// The secondary processor's interrupt input.
			void				SetSecondaryInterrupt(
									std::function<void(bool)> callback)
									{ fSecondaryInterrupt = callback; }
			// The secondary processor entered its external interrupt.
			void				SecondaryInterruptTaken();
			// The ROM left Open Firmware: the dual processor card shows.
			void				RevealDualProcessor() { fRevealed = true; }
			bool				DualProcessorRevealed() const
									{ return fRevealed; }

	// MmioDevice
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "hammerhead"; }

private:
			u32					_Register(u32 offset) const
									{ return LoadBE32(&fRegisters[offset]); }
			u32					_BankBase(int bank) const;
			void				_MapMemory();

			PhysBus&			fBus;
			bool				fDualCpu;
			u8					fRegisters[kSize];
			u8*					fRam;
			std::vector<u32>	fBankSizes;
			std::vector<u32>	fBankOffsets;
			// the ranges mapped now (base, size)
			std::vector<std::pair<u32, u32> > fMapped;
			bool				fSecondaryAsserted;
			bool				fSecondaryTaken;
			bool				fSecondaryClearPending;
			bool				fRevealed;
			std::function<void(bool)> fSecondaryInterrupt;
};

}	// namespace powermac

#endif	// PM_HAMMERHEAD_H
