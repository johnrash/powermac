/*
 * PowerMac emulator - Cuda, the system controller microcontroller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Cuda.h"

#include "../Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace powermac {

namespace {

enum {
	kTreq = 0x08,
	kByteAck = 0x10,
	kTip = 0x20
};

enum {
	kPacketAdb = 0,
	kPacketPseudo = 1,
	kPacketError = 2,
	kPacketTick = 3
};

enum {
	kErrorBadPacket = 1,
	kErrorBadCommand = 2,
	kErrorBadSize = 3,
	kErrorBadParameter = 4
};

enum {
	kWarmStart = 0x00,
	kAutopoll = 0x01,
	kReadMemory = 0x02,
	kGetTime = 0x03,
	kGetPram = 0x07,
	kWriteMemory = 0x08,
	kSetTime = 0x09,
	kPowerDown = 0x0a,
	kSetPowerUpTime = 0x0b,
	kSetPram = 0x0c,
	kMonostableReset = 0x0d,
	kSendDfac = 0x0e,
	kResetSystem = 0x11,
	kSetIpl = 0x12,
	kFileServerFlag = 0x13,
	kSetAutopollRate = 0x14,
	kGetAutopollRate = 0x16,
	kSetDeviceList = 0x19,
	kGetDeviceList = 0x1a,
	kOneSecondMode = 0x1b,
	kSetPowerMessages = 0x21,
	kI2c = 0x22,
	kWakeUp = 0x23,
	kTimerTickle = 0x24,
	kCombinedI2c = 0x25
};

// Where the parameter RAM is in the 68HC05's address space.
const u32 kPramAddress = 0x100;

}	// namespace


Cuda::Cuda(Via6522& via, Scheduler& scheduler, u64 ticksPerSecond)
	:
	fVia(via),
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fDirty(false),
	fTimeBase(0),
	fTimeBaseTicks(0),
	fRestartPending(false)
{
	memset(fPram, 0, sizeof(fPram));
	fShiftTimer.reset(new Timer(scheduler, [this]() { _ShiftTimerFired(); },
		"cuda-shift"));
	fTreqTimer.reset(new Timer(scheduler, [this]() {
		if (fOutputIndex < fOutput.size())
			_SetTreq(true);
	}, "cuda-treq"));
	fPollTimer.reset(new Timer(scheduler, [this]() { _Poll(); },
		"cuda-poll"));
	fSecondTimer.reset(new Timer(scheduler, [this]() { _Second(); },
		"cuda-second"));
	fPowerTimer.reset(new Timer(scheduler, [this]() {
		if (fRestartPending) {
			if (fRestart)
				fRestart();
		} else if (fPowerOff)
			fPowerOff();
	}, "cuda-power"));
	via.SetPeripheral(this);
	Reset();
}


Cuda::~Cuda()
{
	fVia.SetPeripheral(nullptr);
}


void
Cuda::Reset()
{
	fLastLines = kTip | kByteAck;
	fSyncing = false;
	fInput.clear();
	fOutput.clear();
	fDeferred.clear();
	fOutputIndex = 0;
	fOutputOpenEnded = false;
	fShiftTimer->Cancel();
	fTreqTimer->Cancel();
	fPollTimer->Cancel();
	fPowerTimer->Cancel();
	fSecondTimer->Cancel();
	fAutopoll = false;
	fAutopollRate = 11;
	fDeviceList = 0;
	fOneSecondMode = 0;
	fLastSecond = 0;
	fSecondFull = true;
	fFileServer = false;
	fAdb.Reset();
	_SetTreq(false);
}


void
Cuda::SetTime(u32 time)
{
	fTimeBase = time;
	fTimeBaseTicks = fScheduler.Now();
	if (fOneSecondMode != 0) {
		fSecondFull = true;
		_ArmSecond();
	}
}


u32
Cuda::Time() const
{
	return fTimeBase + (u32)((fScheduler.Now() - fTimeBaseTicks)
		/ fTicksPerSecond);
}


void
Cuda::_SetTreq(bool asserted)
{
	fTreqAsserted = asserted;
	fVia.SetPortBInput(kTreq, asserted ? 0 : kTreq);
}


// The shift register interrupt after 'microseconds', with the next byte
// shifted in when 'load' is set.
void
Cuda::_ShiftInterruptIn(u64 microseconds, bool load, u8 byte)
{
	fShiftLoad = load;
	fShiftByte = byte;
	fShiftTimer->ArmIn(microseconds * fTicksPerSecond / 1000000);
}


void
Cuda::_ShiftTimerFired()
{
	if (fShiftLoad) {
		// the last byte of a packet: TREQ goes away with it
		if (!fOutputOpenEnded && fOutputIndex >= fOutput.size())
			_SetTreq(false);
		fVia.ShiftIn(fShiftByte);
	} else
		fVia.ShiftDone();
}


bool
Cuda::_Idle() const
{
	return (fLastLines & kTip) != 0 && fOutputIndex >= fOutput.size()
		&& !fTreqAsserted && !fShiftTimer->IsArmed()
		&& !fTreqTimer->IsArmed();
}


void
Cuda::PortBWritten(u8 value)
{
	u8 lines = value & (kTip | kByteAck);
	if (lines == fLastLines)
		return;
	fLastLines = lines;

	if ((lines & kTip) != 0) {
		if ((lines & kByteAck) != 0) {
			// the end of a transfer
			_SetTreq(false);
			if (fSyncing) {
				// synchronized: no packets of Cuda's own until asked again
				// (Apple's Cuda ERS, as DingusPPC describes it)
				fSyncing = false;
				fAutopoll = false;
				fPollTimer->Cancel();
				fOneSecondMode = 0;
				fSecondTimer->Cancel();
				fDeferred.clear();
			}
			if (!fInput.empty()) {
				_ProcessPacket();
				fInput.clear();
				if (fOutputIndex < fOutput.size())
					fTreqTimer->ArmIn(13 * fTicksPerSecond / 1000000);
			} else {
				// the host read what it wanted (or gave up)
				fOutput.clear();
				fOutputIndex = 0;
				fOutputOpenEnded = false;
				if (!fDeferred.empty()) {
					// a packet of ours waited for a command of the host's
					fOutput.swap(fDeferred);
					fTreqTimer->ArmIn(30 * fTicksPerSecond / 1000000);
				}
			}
		} else {
			// synchronization
			fSyncing = true;
			_SetTreq(true);
			fInput.clear();
			fOutput.clear();
			fOutputIndex = 0;
			fOutputOpenEnded = false;
		}
		_ShiftInterruptIn(61, false);
		return;
	}

	if (fVia.ShiftOut()) {
		// a byte from the host
		if (fInput.empty() && fOutputIndex == 0 && !fOutput.empty()) {
			// it wants to send while a packet of ours waits: it goes
			// first, ours after its command
			PM_LOG(kLogCuda, kLogDebug, "packet %02x %02x deferred for the "
				"host's", fOutput[0], fOutput.size() > 1 ? fOutput[1] : 0);
			fDeferred.swap(fOutput);
			fOutput.clear();
			fTreqTimer->Cancel();
			_SetTreq(false);
		}
		fInput.push_back(fVia.ShiftRegister());
		_ShiftInterruptIn(71, false);
		return;
	}

	// the next byte to the host
	u8 byte = 0;
	if (fOutputIndex < fOutput.size())
		byte = fOutput[fOutputIndex];
	fOutputIndex++;
	_ShiftInterruptIn(88, true, byte);
}


void
Cuda::_Reply(u8 type, u8 status, u8 command, const u8* data, int length)
{
	fOutput.clear();
	fOutput.push_back(type);
	fOutput.push_back(status);
	fOutput.push_back(command);
	if (data != nullptr)
		fOutput.insert(fOutput.end(), data, data + length);
	fOutputIndex = 0;
	fOutputOpenEnded = false;
}


void
Cuda::_Error(u8 code, u8 type, u8 command)
{
	PM_LOG(kLogCuda, kLogInfo, "error %d for packet %02x %02x", code, type,
		command);
	_Reply(kPacketError, code, type);
	fOutput.push_back(command);
}


void
Cuda::_ProcessPacket()
{
	if (Log::Enabled(kLogCuda, kLogDebug)) {
		char text[200];
		int length = 0;
		for (size_t i = 0; i < fInput.size() && length < 180; i++)
			length += snprintf(text + length, sizeof(text) - length, " %02x",
				fInput[i]);
		PM_LOG(kLogCuda, kLogDebug, "packet:%s", text);
	}
	if (fInput.size() < 2) {
		_Error(kErrorBadSize, fInput[0], 0);
		return;
	}
	switch (fInput[0]) {
		case kPacketAdb:
		{
			u8 command = fInput[1];
			std::vector<u8> reply;
			bool answered = fAdb.Command(command, fInput.data() + 2,
				(int)fInput.size() - 2, reply);
			_Reply(kPacketAdb, answered ? 0x00 : 0x02, command,
				reply.data(), (int)reply.size());
			break;
		}
		case kPacketPseudo:
			_PseudoCommand(fInput.data() + 1, (int)fInput.size() - 1);
			break;
		default:
			_Error(kErrorBadPacket, fInput[0], fInput[1]);
			break;
	}
}


void
Cuda::_PseudoCommand(const u8* data, int length)
{
	u8 command = data[0];
	const u8* args = data + 1;
	int count = length - 1;
	switch (command) {
		case kWarmStart:
			fAutopoll = false;
			fOneSecondMode = 0;
			fPollTimer->Cancel();
			fSecondTimer->Cancel();
			break;
		case kAutopoll:
			fAutopoll = count > 0 && args[0] != 0;
			if (fAutopoll)
				fPollTimer->ArmIn(fTicksPerSecond * fAutopollRate / 1000);
			else
				fPollTimer->Cancel();
			break;
		case kReadMemory:
		case kGetPram:
		{
			if (count < 2)
				break;
			u32 address = (args[0] << 8) | args[1];
			if (command == kGetPram) {
				if (address >= kPramSize) {
					_Error(kErrorBadParameter, kPacketPseudo, command);
					return;
				}
				address += kPramAddress;
			}
			_Reply(kPacketPseudo, 0, command);
			for (u32 a = address; a >= kPramAddress
					&& a < kPramAddress + kPramSize; a++)
				fOutput.push_back(fPram[a - kPramAddress]);
			fOutputOpenEnded = true;
			return;
		}
		case kGetTime:
		{
			u32 time = Time();
			u8 bytes[4] = { (u8)(time >> 24), (u8)(time >> 16),
				(u8)(time >> 8), (u8)time };
			_Reply(kPacketPseudo, 0, command, bytes, 4);
			return;
		}
		case kWriteMemory:
		case kSetPram:
		{
			if (count < 2)
				break;
			u32 address = (args[0] << 8) | args[1];
			if (command == kSetPram) {
				if (address >= kPramSize) {
					_Error(kErrorBadParameter, kPacketPseudo, command);
					return;
				}
				address += kPramAddress;
			}
			for (int i = 2; i < count; i++, address++) {
				if (address >= kPramAddress
					&& address < kPramAddress + kPramSize) {
					fPram[address - kPramAddress] = args[i];
					fDirty = true;
				}
			}
			break;
		}
		case kSetTime:
			if (count >= 4) {
				SetTime(((u32)args[0] << 24) | (args[1] << 16)
					| (args[2] << 8) | args[3]);
				fDirty = true;
			}
			break;
		case kPowerDown:
		case kResetSystem:
			PM_LOG(kLogCuda, kLogInfo, "%s", command == kPowerDown
				? "power down" : "restart");
			fRestartPending = command == kResetSystem;
			fPowerTimer->ArmIn(fTicksPerSecond / 1000);
			break;
		case kSetAutopollRate:
			if (count > 0 && args[0] != 0)
				fAutopollRate = args[0];
			break;
		case kGetAutopollRate:
			_Reply(kPacketPseudo, 0, command, &fAutopollRate, 1);
			return;
		case kSetDeviceList:
			if (count >= 2)
				fDeviceList = (args[0] << 8) | args[1];
			break;
		case kGetDeviceList:
		{
			u8 bytes[2] = { (u8)(fDeviceList >> 8), (u8)fDeviceList };
			_Reply(kPacketPseudo, 0, command, bytes, 2);
			return;
		}
		case kOneSecondMode:
			fOneSecondMode = count > 0 ? args[0] : 0;
			PM_LOG(kLogCuda, kLogDebug, "one second mode %d", fOneSecondMode);
			fSecondFull = true;
			if (fOneSecondMode != 0)
				_ArmSecond();
			else
				fSecondTimer->Cancel();
			break;
		case kFileServerFlag:
			fFileServer = count > 0 && args[0] != 0;
			break;
		case kI2c:
		case kCombinedI2c:
			// no I2C devices answer: reads return zeros
			_Reply(kPacketPseudo, 0, command);
			fOutputOpenEnded = count > 0 && (args[count > 2 ? 2 : 0] & 1);
			return;
		case kSetPowerUpTime:
		case kMonostableReset:
		case kSendDfac:
		case kSetIpl:
		case kSetPowerMessages:
		case kWakeUp:
		case kTimerTickle:
			break;
		default:
			_Error(kErrorBadCommand, kPacketPseudo, command);
			return;
	}
	_Reply(kPacketPseudo, 0, command);
}


void
Cuda::InputArrived()
{
	if (fAutopoll && !fPollTimer->IsArmed())
		fPollTimer->ArmIn(fTicksPerSecond / 2000);
}


// The next change of the clock's seconds.
void
Cuda::_ArmSecond()
{
	u64 elapsed = fScheduler.Now() - fTimeBaseTicks;
	fSecondTimer->ArmIn((elapsed / fTicksPerSecond + 1) * fTicksPerSecond
		- elapsed);
}


// A second has gone by: its packet, once the host's transfers are done.
void
Cuda::_Second()
{
	if (fOneSecondMode == 0)
		return;
	if (!_Idle()) {
		// late: this one tells the time
		fSecondFull = true;
		fSecondTimer->ArmIn(fTicksPerSecond / 1000);
		return;
	}
	u32 time = Time();
	if (fSecondFull || fOneSecondMode == 1 || (fLastSecond & 3) == 0) {
		u8 bytes[4] = { (u8)(time >> 24), (u8)(time >> 16), (u8)(time >> 8),
			(u8)time };
		_Reply(kPacketPseudo, 0, kGetTime, bytes, 4);
	} else if (fOneSecondMode == 2)
		_Reply(kPacketPseudo, 0, kGetTime);
	else {
		fOutput.assign(1, kPacketTick);
		fOutputIndex = 0;
		fOutputOpenEnded = false;
	}
	fSecondFull = false;
	fLastSecond = time;
	_SetTreq(true);
	_ShiftInterruptIn(30, false);
	_ArmSecond();
}


void
Cuda::_Poll()
{
	if (!fAutopoll)
		return;
	u64 period = fTicksPerSecond * fAutopollRate / 1000;
	if (!_Idle()) {
		PM_LOG(kLogCuda, kLogTrace, "autopoll: busy (lines %02x, output "
			"%zu/%zu, treq %d, shift %d, treq timer %d)", fLastLines,
			fOutputIndex, fOutput.size(), fTreqAsserted,
			fShiftTimer->IsArmed(), fTreqTimer->IsArmed());
		fPollTimer->ArmIn(period / 4 + 1);
		return;
	}
	AdbDevice* device = fAdb.DeviceWithData(fDeviceList);
	if (device == nullptr) {
		fPollTimer->ArmIn(period);
		return;
	}
	u8 command = (device->Address() << 4) | 0x0c;
	std::vector<u8> data;
	if (device->Talk(0, data)) {
		PM_LOG(kLogCuda, kLogDebug, "autopoll: %02x %02x %02x", command,
			data.size() > 0 ? data[0] : 0, data.size() > 1 ? data[1] : 0);
		_Reply(kPacketAdb, 0x40, command, data.data(), (int)data.size());
		_SetTreq(true);
		_ShiftInterruptIn(30, false);
	}
	fPollTimer->ArmIn(period);
}

}	// namespace powermac
