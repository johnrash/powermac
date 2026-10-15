/*
 * PowerMac emulator - Bandit, Apple's PCI host bridge.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Bandit.h"

#include "../Log.h"

#include <cstdio>

namespace powermac {

Bandit::Bandit(PhysBus& bus, u32 base, int number)
	:
	PciDevice(0x106b, 0x0001, 0x060000, 3),
	fBus(bus),
	fBase(base)
{
	snprintf(fName, sizeof(fName), "bandit%d", number);
	fPci.Attach(kSelfDevice, this);
	fBus.MapDevice(fBase, kWindowSize, this);
	Reset();
}


Bandit::~Bandit()
{
	fBus.Unmap(fBase, kWindowSize);
}


void
Bandit::Reset()
{
	memset(fAddress, 0, sizeof(fAddress));
	// memory enabled, bus master; the "magic" register's coherence bit
	// (Linux sets it when it is clear)
	fConfig[0x04] = 0x06;
	fConfig[0x50] = 0x00;
}


// The device, function and register a configuration data access at
// 'dataOffset' (within the data window) addresses; false for type 1
// cycles and IDSEL patterns that select nothing.
bool
Bandit::_Target(u32 dataOffset, int& device, int& function, u32& reg) const
{
	u32 address = LoadLE32(fAddress);
	if ((address & 1) != 0)
		return false;
	u32 idsel = address & 0xfffff800;
	if (idsel == 0 || (idsel & (idsel - 1)) != 0)
		return false;
	device = __builtin_ctz(idsel);
	function = (address >> 8) & 7;
	reg = (address & 0xfc) | (dataOffset & 3);
	return true;
}


u32
Bandit::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = addr - fBase;
	if (offset < kIoSize) {
		u32 value = fIo.Read(offset, size, initiator,
			fBus.CurrentPc(initiator));
		return SwapSized(value, size);
	}
	if (offset < 0xc00000) {
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fAddress[(offset + i) & 3];
		return value;
	}
	int device;
	int function;
	u32 reg;
	if (!_Target(offset, device, function, reg)) {
		PM_LOG(kLogPci, kLogDebug, "%s: config read%d with address %08x",
			fName, size * 8, LoadLE32(fAddress));
		return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	u32 value = fPci.ConfigRead(device, function, reg, size, initiator);
	return SwapSized(value, size);
}


void
Bandit::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr - fBase;
	if (offset < kIoSize) {
		fIo.Write(offset, SwapSized(value, size), size, initiator,
			fBus.CurrentPc(initiator));
		return;
	}
	if (offset < 0xc00000) {
		for (int i = 0; i < size; i++)
			fAddress[(offset + i) & 3] = (u8)(value >> (8 * (size - 1 - i)));
		PM_LOG(kLogPci, kLogTrace, "%s: config address %08x", fName,
			LoadLE32(fAddress));
		return;
	}
	int device;
	int function;
	u32 reg;
	if (!_Target(offset, device, function, reg)) {
		PM_LOG(kLogPci, kLogDebug,
			"%s: config write%d %x with address %08x", fName, size * 8, value,
			LoadLE32(fAddress));
		return;
	}
	fPci.ConfigWrite(device, function, reg, SwapSized(value, size), size);
}

}	// namespace powermac
