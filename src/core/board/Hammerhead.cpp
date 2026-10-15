/*
 * PowerMac emulator - Hammerhead, the 9500's memory controller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Hammerhead.h"

#include "../Log.h"

#include <algorithm>

namespace powermac {

namespace {

enum {
	kRegId = 0x000,
	kRegBoard = 0x020,
	kRegBusConfig = 0x030,
	kRegArbiter = 0x090,
	kRegWhoAmI = 0x0b0,
	kRegSecondaryInterrupt = 0x0c0,
	kRegL2Cache = 0x0e0,
	kRegBank0 = 0x1c0,
	kRegBankEnd = 0x500
};

const u32 kInterleave = 0x04000000;
const u32 kBaseBit30 = 0x01000000;

// Where no bank answers: the probes of empty banks read a floating bus
// (all ones), without reports of unclaimed accesses.
class EmptyDram : public MmioDevice {
public:
	virtual u32 MmioRead(u32 addr, int size, int initiator)
		{ return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff; }
	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator) {}
	virtual const char* MmioName() const { return "empty-dram"; }
};

EmptyDram sEmptyDram;

}	// namespace


Hammerhead::Hammerhead(PhysBus& bus, bool dualCpu)
	:
	fBus(bus),
	fDualCpu(dualCpu),
	fRam(nullptr),
	fSecondaryAsserted(false),
	fSecondaryTaken(false),
	fSecondaryClearPending(false),
	fRevealed(false)
{
	fBus.MapDevice(kBase, PhysBus::kPageSize, this);
	Reset();
}


Hammerhead::~Hammerhead()
{
	fBus.Unmap(kBase, PhysBus::kPageSize);
}


std::vector<u32>
Hammerhead::BankSizesFor(u32 megabytes)
{
	std::vector<u32> sizes(kBanks, 0);
	u32 remaining = std::max<u32>(8, std::min<u32>(1536, megabytes)) & ~7u;
	int bank = 0;
	while (remaining >= 8 && bank + 1 < kBanks) {
		u32 size = 64;
		while (size * 2 > remaining)
			size /= 2;
		sizes[bank] = sizes[bank + 1] = size << 20;
		remaining -= size * 2;
		bank += 2;
	}
	return sizes;
}


void
Hammerhead::SetMemory(u8* ram, const std::vector<u32>& bankSizes)
{
	fRam = ram;
	fBankSizes = bankSizes;
	fBankSizes.resize(kBanks, 0);
	fBankOffsets.assign(kBanks, 0);
	u32 offset = 0;
	for (int i = 0; i < kBanks; i++) {
		fBankOffsets[i] = offset;
		offset += fBankSizes[i];
	}
	fBus.MapDevice(0, kBanks * kBankWindow, &sEmptyDram);
	fMapped.clear();
	_MapMemory();
}


void
Hammerhead::Reset()
{
	memset(fRegisters, 0, sizeof(fRegisters));
	StoreBE32(&fRegisters[kRegId], 0x39000001);
	StoreBE32(&fRegisters[kRegBoard], 0x40000000);
	// the time bases run
	StoreBE32(&fRegisters[kRegArbiter], 0x04000000);
	fRevealed = false;
	// At reset the banks are where the ROM puts them to size them: bank
	// n at n * 64 MB.
	for (int i = 0; i < kBanks; i++) {
		u32 base = (u32)i * kBankWindow;
		StoreBE32(&fRegisters[kRegBank0 + 0x20 * i],
			(base & 0x40000000) != 0 ? kBaseBit30 : 0);
		StoreBE32(&fRegisters[kRegBank0 + 0x20 * i + 0x10],
			((base >> 22) & 0xff) << 24);
	}
	if (fSecondaryAsserted && fSecondaryInterrupt)
		fSecondaryInterrupt(false);
	fSecondaryAsserted = false;
	fSecondaryTaken = false;
	fSecondaryClearPending = false;
	if (fRam != nullptr)
		_MapMemory();
}


void
Hammerhead::SecondaryInterruptTaken()
{
	fSecondaryTaken = true;
	if (fSecondaryClearPending) {
		fSecondaryClearPending = false;
		fSecondaryAsserted = false;
		PM_LOG(kLogBoard, kLogDebug, "secondary interrupt cleared (late)");
		if (fSecondaryInterrupt)
			fSecondaryInterrupt(false);
	}
}


u32
Hammerhead::_BankBase(int bank) const
{
	u32 high = _Register(kRegBank0 + 0x20 * bank);
	u32 low = _Register(kRegBank0 + 0x20 * bank + 0x10);
	return ((high & kBaseBit30) != 0 ? 0x40000000 : 0)
		| ((low >> 24) << 22);
}


void
Hammerhead::_MapMemory()
{
	std::vector<std::pair<u32, u32> > ranges;
	struct Mapping {
		u32 base;
		u32 size;
		u8* host;
		u32 hostSize;
	};
	std::vector<Mapping> mappings;
	for (int i = 0; i < kBanks; i++) {
		u32 size = fBankSizes[i];
		if (size == 0)
			continue;
		// The second bank of an interleaved pair is part of the first.
		if ((i & 1) != 0
			&& (_Register(kRegBank0 + 0x20 * (i - 1)) & kInterleave) != 0)
			continue;
		if ((i & 1) == 0 && i + 1 < kBanks
			&& (_Register(kRegBank0 + 0x20 * i) & kInterleave) != 0)
			size += fBankSizes[i + 1];
		u32 base = _BankBase(i);
		u32 window = kBankWindow;
		u32 limit = base + std::max(window, size);
		for (int j = 0; j < kBanks; j++) {
			u32 other = _BankBase(j);
			if (j != i && other > base && other < limit)
				limit = other;
		}
		mappings.push_back({ base, limit - base, fRam + fBankOffsets[i],
			size });
	}

	// What was RAM and is not any more reads as an empty bank again.
	for (const auto& range : fMapped)
		fBus.MapDevice(range.first, range.second, &sEmptyDram);
	fMapped.clear();
	for (const Mapping& mapping : mappings) {
		for (u32 offset = 0; offset < mapping.size;
				offset += mapping.hostSize) {
			u32 length = std::min(mapping.hostSize, mapping.size - offset);
			fBus.MapMemory(mapping.base + offset, length, mapping.host, true);
		}
		fMapped.push_back(std::make_pair(mapping.base, mapping.size));
		PM_LOG(kLogBoard, kLogDebug, "RAM %u MB at %08x (window %u MB)",
			mapping.hostSize >> 20, mapping.base, mapping.size >> 20);
	}
}


u32
Hammerhead::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = addr - kBase;
	if (offset + size > kSize)
		return 0xffffffff;
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value = (value << 8) | fRegisters[offset + i];
	if ((offset & ~0xfu) == kRegArbiter && fDualCpu && fRevealed) {
		for (int i = 0; i < size; i++) {
			if (offset + i == kRegArbiter)
				value |= 0x02u << (8 * (size - 1 - i));
		}
	}
	if ((offset & ~0xfu) == kRegWhoAmI && initiator == kInitiatorCpu1) {
		u32 whoAmI = 0x08000000;
		for (int i = 0; i < size; i++) {
			u32 byte = offset + i - kRegWhoAmI;
			if (byte < 4) {
				value |= ((whoAmI >> (8 * (3 - byte))) & 0xff)
					<< (8 * (size - 1 - i));
			}
		}
	}
	PM_LOG(kLogBoard, kLogTrace, "read%d %03x = %x", size * 8, offset, value);
	return value;
}


void
Hammerhead::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr - kBase;
	if (offset + size > kSize)
		return;
	PM_LOG(kLogBoard, kLogDebug, "write%d %03x = %x (cpu%d)", size * 8, offset,
		value, initiator);
	u32 reg = offset & ~0xfu;
	switch (reg) {
		case kRegId:
		case kRegBoard:
		case kRegWhoAmI:
			return;		// read only
		case kRegSecondaryInterrupt:
		{
			// The register's first byte: bit 7 clears the interrupt, its
			// absence asserts it.
			if (offset != reg)
				return;
			u8 byte = (u8)(value >> (8 * (size - 1)));
			bool assert = (byte & 0x80) == 0;
			fRegisters[reg] = byte;
			if (assert) {
				fSecondaryClearPending = false;
				if (!fSecondaryAsserted) {
					fSecondaryAsserted = true;
					fSecondaryTaken = false;
					PM_LOG(kLogBoard, kLogDebug,
						"secondary interrupt asserted (cpu%d)", initiator);
					if (fSecondaryInterrupt)
						fSecondaryInterrupt(true);
				}
			} else if (fSecondaryAsserted) {
				if (initiator != kInitiatorCpu1 && !fSecondaryTaken) {
					// the secondary has not seen it yet
					fSecondaryClearPending = true;
					return;
				}
				fSecondaryAsserted = false;
				PM_LOG(kLogBoard, kLogDebug,
					"secondary interrupt cleared (cpu%d)", initiator);
				if (fSecondaryInterrupt)
					fSecondaryInterrupt(false);
			}
			return;
		}
	}
	u32 before = LoadBE32(&fRegisters[reg]);
	for (int i = 0; i < size; i++)
		fRegisters[offset + i] = (u8)(value >> (8 * (size - 1 - i)));
	if (reg == kRegArbiter) {
		// the dual processor card's presence is not the software's
		fRegisters[reg] &= ~0x02;
	}
	if (reg >= kRegBank0 && reg < kRegBankEnd
		&& LoadBE32(&fRegisters[reg]) != before && fRam != nullptr)
		_MapMemory();
}

}	// namespace powermac
