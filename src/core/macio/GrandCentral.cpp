/*
 * PowerMac emulator - Grand Central, the 9500's I/O controller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "GrandCentral.h"

#include "../Log.h"

#include <cstdio>
#include <set>
#include <string>

namespace powermac {

namespace {

enum {
	kRegEvents = 0x20,
	kRegMask = 0x24,
	kRegClear = 0x28,
	kRegLevels = 0x2c
};

const u32 kModeBit = 0x80000000;

// What sits in each 4 KB page of the window (for the stubs' messages).
const char*
PageName(u32 page)
{
	switch (page) {
		case 0x00: return "interrupts";
		case 0x08: return "dbdma";
		case 0x10: return "53c94";
		case 0x11: return "mace";
		case 0x12: case 0x13: return "escc";
		case 0x14: return "awacs";
		case 0x15: return "swim3";
		case 0x16: case 0x17: return "via";
		case 0x18: return "mesh";
		case 0x19: return "enet-prom";
		case 0x1a: return "board-id";
		case 0x1d: return "nvram-latch";
		case 0x1e: return "iobus-e";
		case 0x1f: return "nvram";
	}
	return "unknown";
}

// A device not emulated yet: keeps what is written, reports each
// register the first time it is used.
class Stub : public MmioDevice {
public:
	Stub(u32 page) : fPage(page) { memset(fData, 0, sizeof(fData)); }

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 offset = addr & 0xfff;
		_Report("read", offset, size, 0);
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fData[(offset + i) & 0xfff];
		return value;
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u32 offset = addr & 0xfff;
		_Report("write", offset, size, value);
		for (int i = 0; i < size; i++)
			fData[(offset + i) & 0xfff] = (u8)(value >> (8 * (size - 1 - i)));
	}

	virtual const char* MmioName() const { return PageName(fPage); }

private:
	void _Report(const char* what, u32 offset, int size, u32 value)
	{
		u32 key = (offset << 1) | (what[0] == 'w' ? 1 : 0);
		if (fSeen.insert(key).second) {
			PM_LOG(kLogBus, kLogInfo, "grand central %s (%05x) %s%d "
				"%03x = %x (not emulated)", PageName(fPage), fPage << 12,
				what, size * 8, offset, value);
		}
	}

	u32 fPage;
	u8 fData[0x1000];
	std::set<u32> fSeen;
};

}	// namespace


GrandCentral::GrandCentral(PhysBus& bus)
	:
	PciDevice(0x106b, 0x0002, 0xff0000, 2),
	fBus(bus),
	fMappedBase(0),
	fCpuInterruptLine(false),
	fNvramLatch(0),
	fNvramDirty(false),
	fPrimaryIpi(false)
{
	DeclareBar(0, kBarMemory, kWindowSize);
	for (u32 page = 0; page < (kWindowSize >> 12); page++) {
		fSlots[page].device = nullptr;
		fSlots[page].base = page << 12;
	}
	static const char* const kDmaNames[kDmaChannels] = {
		"dma-scsi0", "dma-floppy", "dma-enet-tx", "dma-enet-rx",
		"dma-scc-a-tx", "dma-scc-a-rx", "dma-scc-b-tx", "dma-scc-b-rx",
		"dma-audio-out", "dma-audio-in", "dma-scsi1"
	};
	for (int i = 0; i < kDmaChannels; i++) {
		fDma[i].reset(new DbdmaChannel(bus, kDmaNames[i]));
		fDma[i]->SetInterruptCallback([this, i](bool level) {
			SetInterrupt(i, level);
		});
	}
	memset(fNvram, 0, sizeof(fNvram));
	static const u8 kDefaultAddress[6] = { 0x08, 0x00, 0x07, 0x95, 0x00,
		0x01 };
	SetEthernetAddress(kDefaultAddress);
	Reset();
}


GrandCentral::~GrandCentral()
{
	if (fMappedBase != 0)
		fBus.Unmap(fMappedBase, kWindowSize);
}


void
GrandCentral::Reset()
{
	for (auto& channel : fDma)
		channel->Reset();
	fEvents = 0;
	fMask = 0;
	fLevels = 0;
	fDmaLatched = 0;
	fNvramLatch = 0;
	_UpdateCpuInterrupt();
}


void
GrandCentral::AttachDevice(u32 offset, u32 size, MmioDevice* device)
{
	for (u32 page = offset >> 12; page < (offset + size) >> 12; page++) {
		Slot& slot = fSlots[page];
		slot.device = device;
		slot.base = offset;
	}
}


void
GrandCentral::SetEthernetAddress(const u8 address[6])
{
	// The PROM holds the address, then a zero and a checksum byte. Apple's
	// 08:00:07 addresses are stored bit reversed (as the MACE sends the
	// bits); Open Firmware reverses the PROM back only when its first byte
	// is 0x10 (08 reversed), so any other address is stored as it is.
	bool reverse = address[0] == 0x08;
	u8 sum = 0;
	for (int i = 0; i < 6; i++) {
		u8 byte = address[i];
		if (reverse) {
			u8 reversed = 0;
			for (int bit = 0; bit < 8; bit++) {
				if ((byte & (1 << bit)) != 0)
					reversed |= 0x80 >> bit;
			}
			byte = reversed;
		}
		fEthernetProm[i] = byte;
		sum += byte;
	}
	fEthernetProm[6] = 0;
	fEthernetProm[7] = 0xff - sum;
}


void
GrandCentral::BarsChanged()
{
	u32 base = MemoryEnabled() ? BarAddress(0) : 0;
	if (base == fMappedBase)
		return;
	if (fMappedBase != 0)
		fBus.Unmap(fMappedBase, kWindowSize);
	fMappedBase = base;
	if (base != 0) {
		fBus.MapDevice(base, kWindowSize, this);
		PM_LOG(kLogPci, kLogInfo, "grand central at %08x", base);
	}
}


void
GrandCentral::SetInterrupt(int source, bool asserted)
{
	u32 bit = 1u << source;
	bool was = (fLevels & bit) != 0;
	if ((fMask & kModeBit) != 0 ? asserted != was : asserted && !was)
		fEvents |= bit;
	if (source < kDmaChannels && asserted && !was)
		fDmaLatched |= bit;
	if (asserted)
		fLevels |= bit;
	else
		fLevels &= ~bit;
	_UpdateCpuInterrupt();
}


void
GrandCentral::_UpdateCpuInterrupt()
{
	bool line = (fEvents & fMask) != 0;
	if (line == fCpuInterruptLine)
		return;
	fCpuInterruptLine = line;
	PM_LOG(kLogInterrupt, kLogTrace, "cpu interrupt %d (events %08x mask "
		"%08x)", line, fEvents, fMask);
	if (fCpuInterrupt)
		fCpuInterrupt(line);
}


u32
GrandCentral::_OwnRead(u32 offset, int size)
{
	u32 page = offset >> 12;
	u32 within = offset & 0xfff;
	switch (page) {
		case 0x00:
		{
			u32 value = 0;
			switch (within & ~3u) {
				case kRegEvents: value = fEvents & fMask; break;
				case kRegMask: value = fMask; break;
				case kRegClear: value = fEvents; break;
				case kRegLevels: value = fLevels | fDmaLatched; break;
			}
			PM_LOG(kLogInterrupt, kLogTrace, "read %02x = %08x", within,
				value);
			// little endian registers
			u8 bytes[4];
			StoreLE32(bytes, value);
			u32 result = 0;
			for (int i = 0; i < size; i++)
				result = (result << 8) | bytes[(within + i) & 3];
			return result;
		}
		case 0x19:
		{
			if (fPrimaryIpi && within < 4) {
				SetInterrupt(kIrqPrimaryIpi, true);
				SetInterrupt(kIrqPrimaryIpi, false);
			}
			u8 byte = fEthernetProm[(within >> 4) & 7];
			u32 result = 0;
			for (int i = 0; i < size; i++)
				result = (result << 8) | byte;
			return result;
		}
		case 0x1a:
		{
			// the board ID register: 16 bits, low byte first
			static const u8 kBoardId[4] = { 0x3f, 0xe1, 0xff, 0xff };
			u32 result = 0;
			for (int i = 0; i < size; i++)
				result = (result << 8) | kBoardId[(within + i) & 3];
			return result;
		}
		case 0x1d:
			return fNvramLatch;
		case 0x1f:
		{
			u8 byte = fNvram[(fNvramLatch << 5) | ((within >> 4) & 0x1f)];
			u32 result = 0;
			for (int i = 0; i < size; i++)
				result = (result << 8) | byte;
			return result;
		}
	}
	return 0;
}


void
GrandCentral::_OwnWrite(u32 offset, u32 value, int size)
{
	u32 page = offset >> 12;
	u32 within = offset & 0xfff;
	switch (page) {
		case 0x00:
		{
			// little endian: the bytes as they would sit in memory
			u8 bytes[4] = { 0, 0, 0, 0 };
			u32 reg = within & ~3u;
			u32 current = reg == kRegMask ? fMask : 0;
			StoreLE32(bytes, current);
			for (int i = 0; i < size; i++)
				bytes[(within + i) & 3] = (u8)(value >> (8 * (size - 1 - i)));
			u32 data = LoadLE32(bytes);
			PM_LOG(kLogInterrupt, kLogDebug, "write %02x = %08x", reg, data);
			if (reg == kRegMask)
				fMask = data;
			else if (reg == kRegClear) {
				if ((fMask & kModeBit) != 0 && (data & kModeBit) != 0)
					fEvents = 0;
				else
					fEvents &= ~data;
				fDmaLatched &= ~data;
			}
			_UpdateCpuInterrupt();
			return;
		}
		case 0x1d:
		{
			// the byte at the register's address (the ROM writes the
			// latch with stwbrx: the low byte of address >> 5 first)
			fNvramLatch = (u8)(value >> (8 * (size - 1)));
			return;
		}
		case 0x1f:
		{
			u32 address = (fNvramLatch << 5) | ((within >> 4) & 0x1f);
			u8 byte = (u8)(value >> (8 * (size - 1)));
			if (fNvram[address] != byte) {
				fNvram[address] = byte;
				fNvramDirty = true;
			}
			return;
		}
	}
	PM_LOG(kLogBus, kLogDebug, "grand central write to %05x ignored", offset);
}


u32
GrandCentral::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = addr - fMappedBase;
	if (offset >= kWindowSize)
		return 0xffffffff;
	PM_LOG(kLogBus, kLogTrace, "grand central read%d %05x (pc %08x)",
		size * 8, offset, fBus.CurrentPc(initiator));
	if ((offset >> 12) == 0x08) {
		u32 channel = (offset >> 8) & 0xf;
		if (channel < kDmaChannels)
			return fDma[channel]->MmioRead(offset & 0xff, size, initiator);
		return 0;
	}
	Slot& slot = fSlots[offset >> 12];
	if (slot.device != nullptr)
		return slot.device->MmioRead(offset - slot.base, size, initiator);
	u32 page = offset >> 12;
	if (page == 0x00) {
		PM_LOG(kLogInterrupt, kLogTrace, "read%d %03x (pc %08x)", size * 8,
			offset, fBus.CurrentPc(initiator));
	}
	if (page == 0x00 || page == 0x19 || page == 0x1a || page == 0x1d
		|| page == 0x1f)
		return _OwnRead(offset, size);
	fStubs.emplace_back(new Stub(page));
	slot.device = fStubs.back().get();
	slot.base = page << 12;
	return slot.device->MmioRead(offset - slot.base, size, initiator);
}


void
GrandCentral::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr - fMappedBase;
	if (offset >= kWindowSize)
		return;
	PM_LOG(kLogBus, kLogTrace, "grand central write%d %05x = %x (pc %08x)",
		size * 8, offset, value, fBus.CurrentPc(initiator));
	if ((offset >> 12) == 0x08) {
		u32 channel = (offset >> 8) & 0xf;
		if (channel < kDmaChannels)
			fDma[channel]->MmioWrite(offset & 0xff, value, size, initiator);
		return;
	}
	Slot& slot = fSlots[offset >> 12];
	if (slot.device != nullptr) {
		slot.device->MmioWrite(offset - slot.base, value, size, initiator);
		return;
	}
	u32 page = offset >> 12;
	if (page == 0x00 || page == 0x19 || page == 0x1a || page == 0x1d
		|| page == 0x1f) {
		_OwnWrite(offset, value, size);
		return;
	}
	fStubs.emplace_back(new Stub(page));
	slot.device = fStubs.back().get();
	slot.base = page << 12;
	slot.device->MmioWrite(offset - slot.base, value, size, initiator);
}

}	// namespace powermac
