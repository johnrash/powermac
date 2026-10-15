/*
 * PowerMac emulator - MESH, the Macintosh Enhanced SCSI Hardware.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Mesh.h"

#include "../Log.h"

#include <algorithm>

namespace powermac {

namespace {

enum {
	kRegCountLow = 0,
	kRegCountHigh = 1,
	kRegFifo = 2,
	kRegSequence = 3,
	kRegBusStatus0 = 4,
	kRegBusStatus1 = 5,
	kRegFifoCount = 6,
	kRegException = 7,
	kRegError = 8,
	kRegInterruptMask = 9,
	kRegInterrupt = 10,
	kRegSourceId = 11,
	kRegDestinationId = 12,
	kRegSyncParams = 13,
	kRegMeshId = 14,
	kRegSelectionTimeout = 15
};

enum {
	kSeqDma = 0x80,
	kSeqTarget = 0x40,
	kSeqAttention = 0x20
};

enum {
	kCmdArbitrate = 1,
	kCmdSelect = 2,
	kCmdCommand = 3,
	kCmdStatus = 4,
	kCmdDataOut = 5,
	kCmdDataIn = 6,
	kCmdMessageOut = 7,
	kCmdMessageIn = 8,
	kCmdBusFree = 9,
	kCmdReset = 0x0e,
	kCmdFlushFifo = 0x0f
};

enum {
	kIntError = 0x04,
	kIntException = 0x02,
	kIntDone = 0x01
};

enum {
	kExcSelectionTimeout = 0x01,
	kExcPhaseMismatch = 0x02
};

enum {
	kErrSequence = 0x10
};

// the chip in the 7500/8500/9500 (343S1146)
const u8 kMeshId = 0xe2;

}	// namespace


Mesh::Mesh(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fDma(nullptr),
	fInterruptLine(false)
{
	fTimer.reset(new Timer(scheduler, [this]() { _Deliver(); },
		"mesh-done"));
	Reset();
}


Mesh::~Mesh()
{
}


void
Mesh::AttachDevice(int id, std::unique_ptr<ScsiDevice> device)
{
	fDevices[id & 7] = std::move(device);
}


void
Mesh::Reset()
{
	fCount = 0;
	fFifo.clear();
	fSequence = 0;
	fException = 0;
	fError = 0;
	fInterruptMask = 0;
	fInterruptStatus = 0;
	fSourceId = 7;
	fDestinationId = 0;
	fSyncParams = 2;
	fSelectionTimeout = 25;
	fAttention = false;
	fPhase = kPhaseBusFree;
	fTarget = -1;
	fDmaTransfer = false;
	fPioTransfer = false;
	fAckHeld = false;
	fRequestAt = 0;
	fIdentifyLun = -1;
	fDataInPosition = 0;
	fDataOut.clear();
	fMessage = 0;
	fPendingInterrupt = fPendingException = fPendingError = 0;
	fTimer->Cancel();
	for (auto& device : fDevices) {
		if (device)
			device->Reset();
	}
	_UpdateInterrupt();
}


void
Mesh::_UpdateInterrupt()
{
	bool line = (fInterruptStatus & fInterruptMask) != 0;
	if (line == fInterruptLine)
		return;
	fInterruptLine = line;
	if (fInterrupt)
		fInterrupt(line);
}


// A command ends (now or after 'delay' ticks) with these interrupt,
// exception and error bits.
void
Mesh::_Done(u8 interrupt, u8 exception, u8 error, u64 delay)
{
	fPendingInterrupt |= interrupt;
	fPendingException |= exception;
	fPendingError |= error;
	if (delay == 0)
		delay = fTicksPerSecond / 200000;		// 5 us
	fTimer->ArmIn(delay);
}


void
Mesh::_Deliver()
{
	fException |= fPendingException;
	fError |= fPendingError;
	fInterruptStatus |= fPendingInterrupt;
	PM_LOG(kLogScsi, kLogDebug, "mesh: interrupt %02x exception %02x error "
		"%02x phase %d", fInterruptStatus, fException, fError, fPhase);
	fPendingInterrupt = fPendingException = fPendingError = 0;
	_UpdateInterrupt();
}


// The target goes to another phase: it drives the phase lines now and
// asserts REQ a little later, once the MESH lets go of ACK.
void
Mesh::_SetPhase(Phase phase)
{
	fPhase = phase;
	fRequestAt = fScheduler.Now() + fTicksPerSecond / 500000;	// 2 us
}


// A transfer's count is done: the MESH keeps ACK asserted on its last byte
// until the next sequence command, so the target cannot ask for its next
// phase before (drivers wait for REQ to drop after the status byte; the
// ROM's waits for bus free after the command, which lets the target go on
// to the data phase).
void
Mesh::_HoldAck()
{
	fAckHeld = true;
}


void
Mesh::_ReleaseAck()
{
	if (!fAckHeld)
		return;
	fAckHeld = false;
	u64 at = fScheduler.Now() + fTicksPerSecond / 1000000;		// 1 us
	fRequestAt = std::max(fRequestAt, at);
}


bool
Mesh::_Requesting() const
{
	return fTarget >= 0 && fPhase != kPhaseBusFree && !fAckHeld
		&& fScheduler.Now() >= fRequestAt;
}


u8
Mesh::_BusStatus0() const
{
	u8 value = fAttention ? 0x08 : 0;
	if (fTarget >= 0 && fPhase != kPhaseBusFree) {
		value |= (u8)(fPhase & 7);				// MSG C/D I/O
		if (fAckHeld)
			value |= 0x10;						// ACK
	}
	if (_Requesting())
		value |= 0x20;							// REQ
	return value;
}


bool
Mesh::_PhaseIs(Phase phase)
{
	return fTarget >= 0 && fPhase == phase;
}


// A message byte the initiator sends: an IDENTIFY gives the LUN (the
// target ignores the others, say a synchronous transfer request).
void
Mesh::_MessageOut(u8 byte)
{
	if ((byte & 0x80) != 0)
		fIdentifyLun = byte & 7;
}


// The CDB is complete: the target runs the command.
void
Mesh::_Execute()
{
	fCommand = ScsiCommand();
	static const u8 kLengths[8] = { 6, 10, 10, 12, 16, 12, 10, 10 };
	size_t length = fCdb.empty() ? 0 : std::min<size_t>(fCdb.size(),
		kLengths[fCdb[0] >> 5]);
	fCommand.cdbLength = (int)length;
	memcpy(fCommand.cdb, fCdb.data(), std::min<size_t>(length, 16));
	if (fIdentifyLun >= 0)
		fCommand.lun = fIdentifyLun;
	else if (length > 1)
		fCommand.lun = fCommand.cdb[1] >> 5;
	PM_LOG(kLogScsi, kLogDebug, "mesh: target %d command %02x", fTarget,
		length > 0 ? fCommand.cdb[0] : 0);
	fDevices[fTarget]->Execute(fCommand);
	fDataInPosition = 0;
	fDataOut.clear();
	fCdb.clear();
	if (!fCommand.dataIn.empty())
		_SetPhase(kPhaseDataIn);
	else if (fCommand.dataOutLength > 0)
		_SetPhase(kPhaseDataOut);
	else
		_SetPhase(kPhaseStatus);
}


// The data phase ended: data out goes to the device, then status.
void
Mesh::_DataDone()
{
	if (fPhase == kPhaseDataOut && fTarget >= 0)
		fDevices[fTarget]->DataOut(fCommand, fDataOut);
	if (fPhase == kPhaseDataOut || fPhase == kPhaseDataIn)
		_SetPhase(kPhaseStatus);
}


// A data or command transfer through the FIFO (no DMA): moves what it can
// and ends the command when the count is done (or the target changed
// phase). The driver fills or drains the FIFO after starting it.
void
Mesh::_TransferPio()
{
	bool mismatch = false;
	switch (fPhase) {
		case kPhaseMessageOut:
			while (fCount > 0 && !fFifo.empty()) {
				_MessageOut(fFifo.front());
				fFifo.pop_front();
				fCount--;
			}
			if (fCount == 0)
				_SetPhase(kPhaseCommand);
			break;
		case kPhaseCommand:
			while (fCount > 0 && !fFifo.empty()) {
				fCdb.push_back(fFifo.front());
				fFifo.pop_front();
				fCount--;
			}
			if (fCount == 0)
				_Execute();
			break;
		case kPhaseDataOut:
			while (fCount > 0 && !fFifo.empty()
				&& fDataOut.size() < fCommand.dataOutLength) {
				fDataOut.push_back(fFifo.front());
				fFifo.pop_front();
				fCount--;
			}
			if (fDataOut.size() >= fCommand.dataOutLength) {
				_DataDone();
				mismatch = fCount > 0;
			}
			break;
		case kPhaseDataIn:
			while (fCount > 0 && fFifo.size() < 16
				&& fDataInPosition < fCommand.dataIn.size()) {
				fFifo.push_back(fCommand.dataIn[fDataInPosition++]);
				fCount--;
			}
			if (fDataInPosition >= fCommand.dataIn.size()) {
				_DataDone();
				mismatch = fCount > 0;
			}
			break;
		default:
			mismatch = fCount > 0;
			break;
	}
	if (fCount == 0) {
		fPioTransfer = false;
		_HoldAck();
		_Done(kIntDone);
	} else if (mismatch) {
		fPioTransfer = false;
		_Done(kIntException, kExcPhaseMismatch);
	}
}


void
Mesh::_Sequence(u8 value)
{
	fSequence = value;
	u8 command = value & 0x0f;
	bool dma = (value & kSeqDma) != 0;
	fAttention = (value & kSeqAttention) != 0;
	PM_LOG(kLogScsi, kLogDebug, "mesh: sequence %02x count %u phase %d",
		value, fCount, fPhase);
	_ReleaseAck();
	auto mismatch = [this](Phase wanted) {
		if (_PhaseIs(wanted))
			return false;
		_Done(kIntException, kExcPhaseMismatch);
		return true;
	};
	switch (command) {
		case kCmdArbitrate:
			_Done(kIntDone);
			return;
		case kCmdSelect:
		{
			int id = fDestinationId & 7;
			if (id == (fSourceId & 7) || !fDevices[id]) {
				fTarget = -1;
				fPhase = kPhaseBusFree;
				// no answer: the selection timeout (shortened: nothing
				// can tell)
				_Done(kIntException, kExcSelectionTimeout, 0,
					fTicksPerSecond / 1000);
				return;
			}
			fTarget = id;
			fCdb.clear();
			fIdentifyLun = -1;
			_SetPhase(fAttention ? kPhaseMessageOut : kPhaseCommand);
			_Done(kIntDone);
			return;
		}
		case kCmdMessageOut:
			if (mismatch(kPhaseMessageOut))
				return;
			// an identify message, maybe a negotiation the target ignores:
			// taken as the driver fills the FIFO (or by DMA)
			if (dma) {
				fDmaTransfer = true;
				if (fDma != nullptr)
					fDma->Kick();
				return;
			}
			fPioTransfer = true;
			_TransferPio();
			return;
		case kCmdCommand:
			if (mismatch(kPhaseCommand))
				return;
			if (dma) {
				fDmaTransfer = true;
				if (fDma != nullptr)
					fDma->Kick();
				return;
			}
			fPioTransfer = true;
			_TransferPio();
			return;
		case kCmdDataOut:
		case kCmdDataIn:
			if (mismatch(command == kCmdDataIn ? kPhaseDataIn
					: kPhaseDataOut))
				return;
			if (dma) {
				fDmaTransfer = true;
				if (fDma != nullptr)
					fDma->Kick();
				return;
			}
			fPioTransfer = true;
			_TransferPio();
			return;
		case kCmdStatus:
			if (mismatch(kPhaseStatus))
				return;
			fFifo.push_back(fCommand.status);
			fCount = fCount > 0 ? fCount - 1 : 0;
			_SetPhase(kPhaseMessageIn);
			_HoldAck();
			fMessage = 0x00;		// command complete
			_Done(kIntDone);
			return;
		case kCmdMessageIn:
			if (mismatch(kPhaseMessageIn))
				return;
			fFifo.push_back(fMessage);
			fCount = fCount > 0 ? fCount - 1 : 0;
			// after command complete the target leaves the bus
			fPhase = kPhaseBusFree;
			_Done(kIntDone);
			return;
		case kCmdBusFree:
			fPioTransfer = false;
			if (fTarget >= 0 && fPhase != kPhaseBusFree) {
				// the target holds the bus and asks for a phase: the wait
				// for bus free ends with a phase mismatch when it asserts
				// REQ (the ROM's driver waits so for the target's next
				// phase)
				u64 now = fScheduler.Now();
				_Done(kIntException, kExcPhaseMismatch,
					0, fRequestAt > now ? fRequestAt - now : 0);
				return;
			}
			fPhase = kPhaseBusFree;
			fTarget = -1;
			_Done(kIntDone);
			return;
		case kCmdReset:
			// the chip's state goes; the reset ends with command done
			// (BeOS waits for it)
			fTimer->Cancel();
			fFifo.clear();
			fCount = 0;
			fException = fError = 0;
			fInterruptStatus = 0;
			fPendingInterrupt = fPendingException = fPendingError = 0;
			fDmaTransfer = false;
			fPioTransfer = false;
			_UpdateInterrupt();
			_Done(kIntDone);
			return;
		case kCmdFlushFifo:
			fFifo.clear();
			fPioTransfer = false;
			_Done(kIntDone);
			return;
		case 0x0a: case 0x0b: case 0x0c: case 0x0d:
		case 0x00:
			_Done(kIntDone);
			return;
	}
	_Done(kIntError, 0, kErrSequence);
}


u32
Mesh::DmaOutput(const u8* data, u32 length)
{
	if (!fDmaTransfer)
		return 0;
	u32 taken = 0;
	if (fPhase == kPhaseMessageOut) {
		while (taken < length && fCount > 0) {
			_MessageOut(data[taken++]);
			fCount--;
		}
		if (fCount == 0) {
			fDmaTransfer = false;
			_SetPhase(kPhaseCommand);
			_HoldAck();
			_Done(kIntDone);
		}
		return taken;
	}
	if (fPhase == kPhaseCommand) {
		while (taken < length && fCount > 0) {
			fCdb.push_back(data[taken++]);
			fCount--;
		}
		if (fCount == 0) {
			fDmaTransfer = false;
			_Execute();
			_HoldAck();
			_Done(kIntDone);
		}
		return taken;
	}
	if (fPhase != kPhaseDataOut)
		return 0;
	u32 wanted = fCommand.dataOutLength - (u32)fDataOut.size();
	taken = std::min<u32>(length, std::min<u32>(fCount, wanted));
	fDataOut.insert(fDataOut.end(), data, data + taken);
	fCount -= taken;
	bool done = fDataOut.size() >= fCommand.dataOutLength;
	if (fCount == 0 || done) {
		fDmaTransfer = false;
		if (done)
			_DataDone();
		if (fCount == 0) {
			_HoldAck();
			_Done(kIntDone);
		} else
			_Done(kIntException, kExcPhaseMismatch);
	}
	return taken;
}


u32
Mesh::DmaInput(u8* data, u32 length)
{
	if (!fDmaTransfer || fPhase != kPhaseDataIn)
		return 0;
	u32 available = (u32)(fCommand.dataIn.size() - fDataInPosition);
	u32 given = std::min<u32>(length, std::min<u32>(fCount, available));
	memcpy(data, fCommand.dataIn.data() + fDataInPosition, given);
	fDataInPosition += given;
	fCount -= given;
	bool done = fDataInPosition >= fCommand.dataIn.size();
	if (fCount == 0 || done) {
		fDmaTransfer = false;
		if (done)
			_DataDone();
		if (fCount == 0) {
			_HoldAck();
			_Done(kIntDone);
		} else {
			// the target went on to the status phase early
			_Done(kIntException, kExcPhaseMismatch);
			if (fDma != nullptr)
				fDma->EndInput();
		}
	}
	return given;
}


u8
Mesh::_Read(int reg)
{
	switch (reg) {
		case kRegCountLow: return (u8)fCount;
		case kRegCountHigh: return (u8)(fCount >> 8);
		case kRegFifo:
		{
			if (fFifo.empty())
				return 0;
			u8 value = fFifo.front();
			fFifo.pop_front();
			if (fPioTransfer)
				_TransferPio();
			return value;
		}
		case kRegSequence: return fSequence;
		case kRegBusStatus0: return _BusStatus0();
		case kRegBusStatus1: return fTarget >= 0 ? 0x40 : 0;
		case kRegFifoCount: return (u8)fFifo.size();
		case kRegException: return fException;
		case kRegError: return fError;
		case kRegInterruptMask: return fInterruptMask;
		case kRegInterrupt: return fInterruptStatus;
		case kRegSourceId: return fSourceId;
		case kRegDestinationId: return fDestinationId;
		case kRegSyncParams: return fSyncParams;
		case kRegMeshId: return kMeshId;
		case kRegSelectionTimeout: return fSelectionTimeout;
	}
	return 0;
}


void
Mesh::_Write(int reg, u8 value)
{
	switch (reg) {
		case kRegCountLow:
			fCount = (fCount & 0xff00) | value;
			break;
		case kRegCountHigh:
			fCount = (u16)((fCount & 0x00ff) | (value << 8));
			break;
		case kRegFifo:
			if (fFifo.size() < 16)
				fFifo.push_back(value);
			if (fPioTransfer)
				_TransferPio();
			break;
		case kRegSequence:
			_Sequence(value);
			break;
		case kRegException:
			fException &= ~value;
			break;
		case kRegError:
			fError &= ~value;
			break;
		case kRegInterruptMask:
			fInterruptMask = value;
			_UpdateInterrupt();
			break;
		case kRegInterrupt:
			// clearing the exception or error interrupt clears its causes
			if ((value & kIntException) != 0)
				fException = 0;
			if ((value & kIntError) != 0)
				fError = 0;
			fInterruptStatus &= ~value;
			_UpdateInterrupt();
			break;
		case kRegSourceId:
			fSourceId = value;
			break;
		case kRegDestinationId:
			fDestinationId = value;
			break;
		case kRegSyncParams:
			fSyncParams = value;
			break;
		case kRegSelectionTimeout:
			fSelectionTimeout = value;
			break;
	}
}


u32
Mesh::MmioRead(u32 addr, int size, int initiator)
{
	int reg = (addr >> 4) & 15;
	u8 value = _Read(reg);
	// (not the bus status and interrupt registers drivers poll)
	if (reg != kRegBusStatus0 && reg != kRegInterrupt)
		PM_LOG(kLogScsi, kLogTrace, "mesh: read %x = %02x", reg, value);
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | value;
	return result;
}


void
Mesh::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	PM_LOG(kLogScsi, kLogTrace, "mesh: write %x = %02x", (addr >> 4) & 15,
		(u8)(value >> (8 * (size - 1))));
	_Write((addr >> 4) & 15, (u8)(value >> (8 * (size - 1))));
}

}	// namespace powermac
