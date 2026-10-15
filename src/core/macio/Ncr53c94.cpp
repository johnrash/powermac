/*
 * PowerMac emulator - NCR 53C94 SCSI controller (initiator role), as in
 * Grand Central.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Ncr53c94.h"

#include "../Log.h"

#include <algorithm>

namespace powermac {

// Registers (read / write)
enum {
	kRegCountLow = 0,		// current / start transfer count
	kRegCountHigh = 1,
	kRegFifo = 2,
	kRegCommand = 3,
	kRegStatus = 4,			// status / destination ID
	kRegInterrupt = 5,		// interrupt status / selection timeout
	kRegSequence = 6,		// sequence step / synchronous period
	kRegFifoFlags = 7,		// FIFO flags / synchronous offset
	kRegConfig1 = 8,
	kRegClockFactor = 9,
	kRegTest = 10,
	kRegConfig2 = 11,
	kRegConfig3 = 12,
	kRegConfig4 = 13,
	kRegCountHigh2 = 14		// bits 16-23 of the count (53CF94)
};

// Status register
enum {
	kStatusInterrupt = 0x80,
	kStatusGrossError = 0x40,
	kStatusCountZero = 0x10
};

// Interrupt status register
enum {
	kIsrBusReset = 0x80,
	kIsrIllegalCommand = 0x40,
	kIsrDisconnect = 0x20,
	kIsrBusService = 0x10,
	kIsrFunctionComplete = 0x08
};

static const size_t kFifoSize = 16;
static const u8 kConfig1DisableResetInterrupt = 0x40;


Ncr53c94::Ncr53c94(Scheduler& scheduler, u64 ticksPerSecond, u32 clockHz)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fClockHz(clockHz),
	fDma(nullptr)
{
	fInterruptTimer.reset(new Timer(scheduler, [this]() {
		_DeliverInterrupt();
	}, "53c94"));
	Reset();
}


Ncr53c94::~Ncr53c94()
{
	fInterruptTimer.reset();
}


void
Ncr53c94::AttachDevice(int id, std::unique_ptr<ScsiDevice> device)
{
	if (id >= 0 && id < 8)
		fDevices[id] = std::move(device);
}


void
Ncr53c94::Reset()
{
	fFifo.clear();
	fStartCount = 0;
	fCount = 0;
	fDmaActive = false;
	fStatus = 0;
	fInterruptStatus = 0;
	fSequenceStep = 0;
	fDestination = 0;
	fTimeout = 0;
	fConfig1 = 7;
	fConfig2 = 0;
	fConfig3 = 0;
	fClockFactor = 2;
	fCommandRegister = 0;
	fPhase = kPhaseBusFree;
	fTarget = -1;
	fDataInPosition = 0;
	fDataOut.clear();
	fPendingIsr = 0;
	fExtraResetInterrupts = 0;
	if (fInterruptTimer)
		fInterruptTimer->Cancel();
	_UpdateIrq();
}


u32
Ncr53c94::MmioRead(u32 addr, int size, int initiator)
{
	u8 value = Read((addr >> 4) & 15);
	PM_LOG(kLogScsi, kLogTrace, "53c94: read %x = %02x", (addr >> 4) & 15,
		value);
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | value;
	return result;
}


void
Ncr53c94::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	PM_LOG(kLogScsi, kLogTrace, "53c94: write %x = %02x", (addr >> 4) & 15,
		(u8)(value >> (8 * (size - 1))));
	Write((addr >> 4) & 15, (u8)(value >> (8 * (size - 1))));
}


u8
Ncr53c94::Read(int reg)
{
	switch (reg) {
		case kRegCountLow:
			return fCount & 0xff;
		case kRegCountHigh:
			return (fCount >> 8) & 0xff;
		case kRegCountHigh2:
			return (fCount >> 16) & 0xff;
		case kRegFifo:
		{
			if (fFifo.empty())
				return 0;
			u8 value = fFifo.front();
			fFifo.pop_front();
			return value;
		}
		case kRegCommand:
			return fCommandRegister;
		case kRegStatus:
			return fStatus | (fPhase == kPhaseBusFree ? 0 : fPhase);
		case kRegInterrupt:
		{
			// reading clears the interrupt
			u8 value = fInterruptStatus;
			fInterruptStatus = 0;
			fStatus &= ~(kStatusInterrupt | kStatusGrossError);
			_UpdateIrq();
			if (fExtraResetInterrupts > 0) {
				fExtraResetInterrupts--;
				_RaiseInterrupt(kIsrBusReset, fTicksPerSecond / 100000);
			}
			return value;
		}
		case kRegSequence:
			return fSequenceStep;
		case kRegFifoFlags:
			return _FifoFlags();
		case kRegConfig1:
			return fConfig1;
		case kRegConfig2:
			return fConfig2;
		case kRegConfig3:
			return fConfig3;
	}
	return 0;
}


void
Ncr53c94::Write(int reg, u8 byte)
{
	switch (reg) {
		case kRegCountLow:
			fStartCount = (fStartCount & 0xffff00) | byte;
			return;
		case kRegCountHigh:
			fStartCount = (fStartCount & 0xff00ff) | (byte << 8);
			return;
		case kRegCountHigh2:
			fStartCount = (fStartCount & 0x00ffff) | ((u32)byte << 16);
			return;
		case kRegFifo:
			if (fFifo.size() < kFifoSize)
				fFifo.push_back(byte);
			return;
		case kRegCommand:
			_Command(byte);
			return;
		case kRegStatus:
			fDestination = byte & 7;
			return;
		case kRegInterrupt:
			fTimeout = byte;
			return;
		case kRegConfig1:
			fConfig1 = byte;
			return;
		case kRegClockFactor:
			fClockFactor = byte & 7;
			return;
		case kRegConfig2:
			fConfig2 = byte;
			return;
		case kRegConfig3:
			fConfig3 = byte;
			return;
	}
}


u32
Ncr53c94::_TransferCount() const
{
	return fStartCount == 0 ? 0x10000 : fStartCount;
}


u32
Ncr53c94::DmaInput(u8* data, u32 length)
{
	if (!fDmaActive || fPhase != kPhaseDataIn)
		return 0;
	u32 available = (u32)(fCommand.dataIn.size() - fDataInPosition);
	u32 given = std::min<u32>(length, std::min<u32>(fCount, available));
	memcpy(data, fCommand.dataIn.data() + fDataInPosition, given);
	fDataInPosition += given;
	fCount -= given;
	if (fCount == 0 || fDataInPosition >= fCommand.dataIn.size())
		_DmaDone();
	return given;
}


u32
Ncr53c94::DmaOutput(const u8* data, u32 length)
{
	if (!fDmaActive || fPhase != kPhaseDataOut)
		return 0;
	u32 wanted = fCommand.dataOutLength - (u32)fDataOut.size();
	u32 taken = std::min<u32>(length, std::min<u32>(fCount, wanted));
	fDataOut.insert(fDataOut.end(), data, data + taken);
	fCount -= taken;
	if (fCount == 0 || fDataOut.size() >= fCommand.dataOutLength)
		_DmaDone();
	return taken;
}


// The end of the count, or of the data: the next phase interrupts.
void
Ncr53c94::_DmaDone()
{
	bool dataDone = fPhase == kPhaseDataIn
		? fDataInPosition >= fCommand.dataIn.size()
		: fDataOut.size() >= fCommand.dataOutLength;
	fDmaActive = false;
	if (fCount == 0)
		fStatus |= kStatusCountZero;
	if (dataDone)
		_DataDone();
	if (fCount != 0 && fDma != nullptr && fPhase != kPhaseDataOut)
		fDma->EndInput();
	_RaiseInterrupt(kIsrBusService, fTicksPerSecond / 200000);
}


u8
Ncr53c94::_FifoFlags() const
{
	size_t count = fFifo.size();
	return (u8)((fSequenceStep << 5) | std::min<size_t>(count, 0x1f));
}


void
Ncr53c94::_Command(u8 command)
{
	fCommandRegister = command;
	bool dma = (command & 0x80) != 0;
	if (dma) {
		fCount = _TransferCount();
		fStatus &= ~kStatusCountZero;
	}
	switch (command & 0x7f) {
		case 0x00:		// NOP
			return;
		case 0x01:		// flush FIFO
			fFifo.clear();
			return;
		case 0x02:		// reset chip
			Reset();
			return;
		case 0x03:		// reset SCSI bus
			_ResetBus();
			return;
		case 0x10:		// transfer information
			_TransferInformation(dma);
			return;
		case 0x11:		// initiator command complete sequence
			_InitiatorCommandComplete();
			return;
		case 0x12:		// message accepted
			_MessageAccepted();
			return;
		case 0x18:		// transfer pad
			fDmaActive = false;
			_DataDone();
			return;
		case 0x1a:		// set ATN
		case 0x1b:		// reset ATN
		case 0x44:		// enable selection / reselection
		case 0x45:		// disable selection / reselection
			return;
		case 0x41:		// select without ATN
			_Select(false);
			return;
		case 0x42:		// select with ATN
		case 0x46:		// select with ATN3
			_Select(true);
			return;
	}
	PM_LOG(kLogScsi, kLogWarning, "53c94: unsupported command %02x",
		command);
	_RaiseInterrupt(kIsrIllegalCommand, 0);
}


void
Ncr53c94::_ResetBus()
{
	PM_LOG(kLogScsi, kLogDebug, "53c94: SCSI bus reset");
	for (auto& device : fDevices) {
		if (device)
			device->Reset();
	}
	fPhase = kPhaseBusFree;
	fTarget = -1;
	fDmaActive = false;
	if ((fConfig1 & kConfig1DisableResetInterrupt) == 0)
		_RaiseInterrupt(kIsrBusReset, fTicksPerSecond / 40000);
}


void
Ncr53c94::_Select(bool attention)
{
	int id = fDestination & 7;
	std::vector<u8> bytes(fFifo.begin(), fFifo.end());
	fFifo.clear();
	fDmaActive = false;
	if (id == (fConfig1 & 7) || !fDevices[id]) {
		// selection timeout: 8192 clocks per unit, times the clock factor
		u64 clocks = (u64)std::max<u8>(fTimeout, 1) * 8192
			* (fClockFactor == 0 ? 8 : fClockFactor);
		fSequenceStep = 0;
		fPhase = kPhaseBusFree;
		_RaiseInterrupt(kIsrDisconnect, clocks * fTicksPerSecond / fClockHz);
		return;
	}
	fTarget = id;
	fCommand = ScsiCommand();
	size_t start = 0;
	if (attention && !bytes.empty()) {
		// the identify message
		fCommand.lun = bytes[0] & 7;
		start = 1;
	}
	size_t length = bytes.size() - start;
	if (length > 0) {
		// the CDB group gives its length
		static const u8 kLengths[8] = { 6, 10, 10, 12, 16, 12, 10, 10 };
		length = std::min<size_t>(length, kLengths[bytes[start] >> 5]);
	}
	fCommand.cdbLength = (int)length;
	memcpy(fCommand.cdb, bytes.data() + start, std::min<size_t>(length, 16));
	if (!attention && length > 1)
		fCommand.lun = fCommand.cdb[1] >> 5;
	PM_LOG(kLogScsi, kLogDebug, "53c94: target %d command %02x (%zu bytes)",
		id, length > 0 ? fCommand.cdb[0] : 0, length);
	fDevices[id]->Execute(fCommand);
	fDataInPosition = 0;
	fDataOut.clear();
	if (!fCommand.dataIn.empty())
		fPhase = kPhaseDataIn;
	else if (fCommand.dataOutLength > 0)
		fPhase = kPhaseDataOut;
	else
		fPhase = kPhaseStatus;
	fSequenceStep = 4;
	_RaiseInterrupt(kIsrBusService | kIsrFunctionComplete,
		fTicksPerSecond / 100000);
}


void
Ncr53c94::_TransferInformation(bool dma)
{
	switch (fPhase) {
		case kPhaseDataIn:
			if (dma) {
				// the data moves through the DBDMA channel
				fDmaActive = true;
				if (fDma != nullptr)
					fDma->Kick();
				return;
			}
			if (fDataInPosition < fCommand.dataIn.size())
				fFifo.push_back(fCommand.dataIn[fDataInPosition++]);
			if (fDataInPosition >= fCommand.dataIn.size())
				_DataDone();
			_RaiseInterrupt(kIsrBusService, 0);
			return;
		case kPhaseDataOut:
			if (dma) {
				fDmaActive = true;
				if (fDma != nullptr)
					fDma->Kick();
				return;
			}
			while (!fFifo.empty()
				&& fDataOut.size() < fCommand.dataOutLength) {
				fDataOut.push_back(fFifo.front());
				fFifo.pop_front();
			}
			if (fDataOut.size() >= fCommand.dataOutLength)
				_DataDone();
			_RaiseInterrupt(kIsrBusService, 0);
			return;
		case kPhaseStatus:
			fFifo.push_back(fCommand.status);
			fPhase = kPhaseMessageIn;
			_RaiseInterrupt(kIsrBusService, 0);
			return;
		case kPhaseMessageIn:
			fFifo.push_back(0x00);		// command complete
			_RaiseInterrupt(kIsrFunctionComplete, 0);
			return;
		default:
			PM_LOG(kLogScsi, kLogWarning, "53c94: transfer information in "
				"phase %d", fPhase);
			_RaiseInterrupt(kIsrDisconnect, 0);
			return;
	}
}


// The data phase ended: data out goes to the device, then status.
void
Ncr53c94::_DataDone()
{
	if (fPhase == kPhaseDataOut && fTarget >= 0)
		fDevices[fTarget]->DataOut(fCommand, fDataOut);
	if (fPhase == kPhaseDataOut || fPhase == kPhaseDataIn)
		fPhase = kPhaseStatus;
}


void
Ncr53c94::_InitiatorCommandComplete()
{
	if (fPhase != kPhaseStatus) {
		PM_LOG(kLogScsi, kLogWarning, "53c94: command complete in phase %d",
			fPhase);
		if (fPhase == kPhaseDataIn || fPhase == kPhaseDataOut)
			_DataDone();
	}
	fFifo.push_back(fCommand.status);
	fFifo.push_back(0x00);		// command complete message
	fPhase = kPhaseMessageIn;
	_RaiseInterrupt(kIsrFunctionComplete, 0);
}


void
Ncr53c94::_MessageAccepted()
{
	// after command complete the target leaves the bus
	fPhase = kPhaseBusFree;
	fTarget = -1;
	_RaiseInterrupt(kIsrDisconnect, 0);
}


void
Ncr53c94::_RaiseInterrupt(u8 isr, u64 delay)
{
	if (delay == 0) {
		fInterruptStatus |= isr;
		fStatus |= kStatusInterrupt;
		_UpdateIrq();
		return;
	}
	fPendingIsr |= isr;
	if (!fInterruptTimer->IsArmed())
		fInterruptTimer->ArmIn(delay);
}


void
Ncr53c94::_DeliverInterrupt()
{
	u8 isr = fPendingIsr;
	fPendingIsr = 0;
	if (isr != 0)
		_RaiseInterrupt(isr, 0);
}


void
Ncr53c94::_UpdateIrq()
{
	if (fIrq)
		fIrq((fStatus & kStatusInterrupt) != 0);
}

}	// namespace powermac
