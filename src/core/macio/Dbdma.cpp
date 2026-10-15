/*
 * PowerMac emulator - a DBDMA channel (Apple's descriptor based DMA).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Dbdma.h"

#include "../Log.h"

#include <algorithm>

namespace powermac {

namespace {

enum {
	kRegControl = 0x00,
	kRegStatus = 0x04,
	kRegCommandPointerHigh = 0x08,
	kRegCommandPointer = 0x0c,
	kRegInterruptSelect = 0x10,
	kRegBranchSelect = 0x14,
	kRegWaitSelect = 0x18
};

enum {
	kOutputMore = 0,
	kOutputLast = 1,
	kInputMore = 2,
	kInputLast = 3,
	kStoreQuad = 4,
	kLoadQuad = 5,
	kNop = 6,
	kStop = 7
};

const u32 kWritableBits = 0xf0ff;	// RUN, PAUSE, FLUSH, WAKE, S7-S0
const u32 kChunk = 4096;

}	// namespace


DbdmaChannel::DbdmaChannel(PhysBus& bus, const char* name)
	:
	fBus(bus),
	fName(name),
	fDevice(nullptr)
{
	Reset();
}


void
DbdmaChannel::Reset()
{
	fStatus = 0;
	fCommandPointer = 0;
	fInterruptSelect = 0;
	fBranchSelect = 0;
	fWaitSelect = 0;
	fHaveDescriptor = false;
	fDone = 0;
	fInputEnded = false;
	fWaiting = false;
	fStopped = false;
	fProcessing = false;
}


bool
DbdmaChannel::_Condition(u32 select) const
{
	u32 mask = (select >> 16) & 0xff;
	return (fStatus & mask) == (select & mask);
}


bool
DbdmaChannel::_Test(int field, u32 select) const
{
	switch (field) {
		case 1: return _Condition(select);
		case 2: return !_Condition(select);
		case 3: return true;
	}
	return false;
}


void
DbdmaChannel::_Pulse()
{
	if (fInterrupt) {
		fInterrupt(true);
		fInterrupt(false);
	}
}


bool
DbdmaChannel::_Fetch()
{
	u8 bytes[16];
	fBus.DmaRead(fCommandPointer, bytes, sizeof(bytes));
	fDescriptor.requestCount = LoadLE16(bytes);
	fDescriptor.command = LoadLE16(bytes + 2);
	fDescriptor.address = LoadLE32(bytes + 4);
	fDescriptor.dependent = LoadLE32(bytes + 8);
	fHaveDescriptor = true;
	fDone = 0;
	fInputEnded = false;
	fStatus |= kActive;
	PM_LOG(kLogDma, kLogTrace, "%s: %08x: command %04x count %u address "
		"%08x dep %08x", fName, fCommandPointer, fDescriptor.command,
		fDescriptor.requestCount, fDescriptor.address, fDescriptor.dependent);
	return true;
}


bool
DbdmaChannel::_Complete()
{
	u16 command = fDescriptor.command;
	u8 tail[4];
	StoreLE16(tail, (u16)(fDescriptor.requestCount - fDone));
	StoreLE16(tail + 2, (u16)fStatus);
	fBus.DmaWrite(fCommandPointer + 12, tail, sizeof(tail));

	if (_Test((command >> 4) & 3, fInterruptSelect))
		_Pulse();
	if (_Test((command >> 2) & 3, fBranchSelect)) {
		fCommandPointer = fDescriptor.dependent & ~15u;
		fStatus |= kBranchTaken;
	} else {
		fCommandPointer += 16;
		fStatus &= ~kBranchTaken;
	}
	fHaveDescriptor = false;
	fDone = 0;
	fInputEnded = false;
	fWaiting = _Test(command & 3, fWaitSelect);
	return !fWaiting;
}


// STORE_QUAD and LOAD_QUAD move 1, 2 or 4 bytes (the request count, 4 when
// larger, 2 for 3) between the descriptor's command dependent field and an
// address, in the order of their addresses. Key 6 (system) is any physical
// address, device registers included (Apple's MACE driver reads the chip's
// transmit status this way after each frame).
void
DbdmaChannel::_Quad(bool load)
{
	u32 count = fDescriptor.requestCount >= 4 ? 4
		: fDescriptor.requestCount >= 2 ? 2 : 1;
	u32 address = fDescriptor.address & ~(count - 1);
	u32 key = (fDescriptor.command >> 8) & 7;
	if (key != 6) {
		PM_LOG(kLogDma, kLogInfo, "%s: %s with key %u not supported", fName,
			load ? "LOAD_QUAD" : "STORE_QUAD", key);
		fDone = fDescriptor.requestCount;
		return;
	}
	u8 data[4];
	if (load) {
		switch (count) {
			case 1: data[0] = fBus.Read8(address, kInitiatorDma); break;
			case 2: StoreBE16(data, fBus.Read16(address, kInitiatorDma)); break;
			default: StoreBE32(data, fBus.Read32(address, kInitiatorDma));
		}
		fBus.DmaWrite(fCommandPointer + 8, data, count);
	} else {
		StoreLE32(data, fDescriptor.dependent);
		switch (count) {
			case 1: fBus.Write8(address, data[0], kInitiatorDma); break;
			case 2: fBus.Write16(address, LoadBE16(data), kInitiatorDma); break;
			default: fBus.Write32(address, LoadBE32(data), kInitiatorDma);
		}
	}
	PM_LOG(kLogDma, kLogTrace, "%s: %s %u bytes at %08x: %02x %02x %02x %02x",
		fName, load ? "LOAD_QUAD" : "STORE_QUAD", count, address, data[0],
		count > 1 ? data[1] : 0, count > 2 ? data[2] : 0,
		count > 2 ? data[3] : 0);
	fDone = fDescriptor.requestCount;
}


void
DbdmaChannel::Kick()
{
	_Process();
}


void
DbdmaChannel::EndInput()
{
	fInputEnded = true;
	_Process();
}


void
DbdmaChannel::SetDeviceStatus(u8 mask, u8 bits)
{
	fStatus = (fStatus & ~(u32)mask) | (bits & mask);
	if (fWaiting)
		_Process();
}


void
DbdmaChannel::_Process()
{
	if (fProcessing)
		return;
	fProcessing = true;
	u8 buffer[kChunk];
	while ((fStatus & kRun) != 0 && (fStatus & (kPause | kDead)) == 0
		&& !fStopped) {
		if (fWaiting) {
			if (_Test(fDescriptor.command & 3, fWaitSelect))
				break;
			fWaiting = false;
		}
		if (!fHaveDescriptor)
			_Fetch();
		u32 command = fDescriptor.command >> 12;
		u32 remaining = fDescriptor.requestCount - fDone;
		bool blocked = false;
		switch (command) {
			case kOutputMore:
			case kOutputLast:
			{
				if (remaining == 0)
					break;
				u32 length = std::min(remaining, kChunk);
				fBus.DmaRead(fDescriptor.address + fDone, buffer, length);
				u32 taken = fDevice != nullptr
					? fDevice->DmaOutput(buffer, length) : length;
				fDone += taken;
				blocked = taken == 0;
				break;
			}
			case kInputMore:
			case kInputLast:
			{
				if (remaining == 0 || fInputEnded)
					break;
				u32 length = std::min(remaining, kChunk);
				u32 given = fDevice != nullptr
					? fDevice->DmaInput(buffer, length) : 0;
				if (given > 0)
					fBus.DmaWrite(fDescriptor.address + fDone, buffer, given);
				fDone += given;
				blocked = given == 0 && !fInputEnded;
				break;
			}
			case kStoreQuad:
			case kLoadQuad:
				_Quad(command == kLoadQuad);
				break;
			case kNop:
				break;
			case kStop:
				// idle until WAKE (or RUN again) fetches the command at
				// the command pointer anew; the device's kicks do not
				fStatus &= ~kActive;
				fHaveDescriptor = false;
				fStopped = true;
				blocked = true;
				break;
			default:
				PM_LOG(kLogDma, kLogWarning, "%s: bad command %04x at %08x",
					fName, fDescriptor.command, fCommandPointer);
				fStatus |= kDead;
				fStatus &= ~kActive;
				_Pulse();
				blocked = true;
				break;
		}
		if (blocked)
			break;
		bool transfer = command <= kInputLast;
		bool finished = !transfer || fDone >= fDescriptor.requestCount
			|| fInputEnded;
		if (finished && command == kOutputLast && fDevice != nullptr)
			fDevice->DmaOutputLast();
		if (finished && !_Complete())
			break;
	}
	fProcessing = false;
}


u32
DbdmaChannel::_Read32(u32 offset) const
{
	switch (offset) {
		case kRegControl: return 0;
		case kRegStatus: return fStatus;
		case kRegCommandPointer: return fCommandPointer;
		case kRegInterruptSelect: return fInterruptSelect;
		case kRegBranchSelect: return fBranchSelect;
		case kRegWaitSelect: return fWaitSelect;
	}
	return 0;
}


void
DbdmaChannel::_Write32(u32 offset, u32 value)
{
	switch (offset) {
		case kRegControl:
		{
			u32 mask = (value >> 16) & kWritableBits;
			u32 old = fStatus;
			fStatus = (fStatus & ~mask) | (value & mask);
			PM_LOG(kLogDma, kLogDebug, "%s: control %08x: status %04x -> "
				"%04x", fName, value, old, fStatus);
			if ((old & kRun) != 0 && (fStatus & kRun) == 0) {
				// stopped: what is in progress is abandoned
				fStatus &= ~(kActive | kDead);
				fHaveDescriptor = false;
				fWaiting = false;
				fStopped = false;
				return;
			}
			if ((fStatus & kFlush) != 0) {
				fStatus &= ~kFlush;
				if (fHaveDescriptor) {
					u32 command = fDescriptor.command >> 12;
					if (command == kInputMore || command == kInputLast)
						fInputEnded = true;
					if (command <= kInputLast) {
						// the partial count is written back (still going):
						// the SWIM3 drivers read it after a sector
						u8 tail[4];
						StoreLE16(tail, (u16)(fDescriptor.requestCount
							- fDone));
						StoreLE16(tail + 2, (u16)fStatus);
						fBus.DmaWrite(fCommandPointer + 12, tail,
							sizeof(tail));
					}
				}
			}
			if ((fStatus & kWake) != 0) {
				fStatus &= ~kWake;
				fStopped = false;
			}
			if ((fStatus & kRun) != 0) {
				if ((old & kRun) == 0) {
					fHaveDescriptor = false;
					fWaiting = false;
					fStopped = false;
				}
				_Process();
			}
			return;
		}
		case kRegCommandPointer:
			if ((fStatus & kActive) == 0)
				fCommandPointer = value & ~15u;
			return;
		case kRegInterruptSelect:
			PM_LOG(kLogDma, kLogDebug, "%s: interrupt select %08x", fName,
				value);
			fInterruptSelect = value;
			return;
		case kRegBranchSelect:
			PM_LOG(kLogDma, kLogDebug, "%s: branch select %08x", fName,
				value);
			fBranchSelect = value;
			return;
		case kRegWaitSelect:
			PM_LOG(kLogDma, kLogDebug, "%s: wait select %08x", fName, value);
			fWaitSelect = value;
			if (fWaiting)
				_Process();
			return;
	}
}


u32
DbdmaChannel::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = addr & 0xff;
	u8 bytes[4];
	StoreLE32(bytes, _Read32(offset & ~3u));
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | bytes[(offset + i) & 3];
	return result;
}


void
DbdmaChannel::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr & 0xff;
	u32 reg = offset & ~3u;
	u8 bytes[4];
	StoreLE32(bytes, _Read32(reg));
	if (reg == kRegControl)
		memset(bytes, 0, sizeof(bytes));
	for (int i = 0; i < size; i++)
		bytes[(offset + i) & 3] = (u8)(value >> (8 * (size - 1 - i)));
	_Write32(reg, LoadLE32(bytes));
}

}	// namespace powermac
