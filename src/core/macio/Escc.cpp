/*
 * PowerMac emulator - the ESCC (Z85C30 serial communications controller).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Escc.h"

#include "../Log.h"
#include "../host/SerialBackend.h"

#include <algorithm>

namespace powermac {

namespace {

// RR0
enum {
	kRr0RxAvailable = 0x01,
	kRr0ZeroCount = 0x02,
	kRr0TxEmpty = 0x04,
	kRr0Dcd = 0x08,
	kRr0SyncHunt = 0x10,
	kRr0Cts = 0x20,
	kRr0TxUnderrun = 0x40
};

// RR3 (channel A only): interrupt pending bits
enum {
	kIpBExternal = 0x01,
	kIpBTx = 0x02,
	kIpBRx = 0x04,
	kIpAExternal = 0x08,
	kIpATx = 0x10,
	kIpARx = 0x20
};

const int kRxFifoSize = 3;

}	// namespace


Escc::Escc(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fInterruptLine(false)
{
	for (int i = 0; i < 2; i++) {
		Channel& channel = fChannels[i];
		channel.backend = nullptr;
		channel.txTimer.reset(new Timer(scheduler,
			[this, i]() { _TransmitDone(i); }, "escc-tx"));
		channel.pollTimer.reset(new Timer(scheduler,
			[this, i]() { _Poll(i); }, "escc-poll"));
	}
	Reset();
}


Escc::~Escc()
{
}


void
Escc::Reset()
{
	_ResetChannel(0, true);
	_ResetChannel(1, true);
	fChannels[0].wr[9] = 0xc0;
	_UpdateInterrupt();
}


void
Escc::_ResetChannel(int index, bool hardware)
{
	Channel& channel = fChannels[index];
	u8 wr2 = channel.wr[2];
	u8 wr9 = channel.wr[9];
	memset(channel.wr, 0, sizeof(channel.wr));
	if (!hardware) {
		channel.wr[2] = wr2;
		channel.wr[9] = wr9;
	}
	channel.wr[4] = 0x04;		// 1 stop bit
	channel.wr[11] = 0x08;
	channel.wr[14] = 0x30;
	channel.wr[15] = 0xf8;
	channel.pointer = 0;
	channel.rx.clear();
	channel.txBusy = false;
	channel.txFull = false;
	channel.txInterruptPending = false;
	channel.rxInterruptPending = false;
	channel.extInterruptPending = false;
	channel.rxOverrun = false;
	channel.txUnderrun = true;		// set by a reset
	channel.lastStatus = 0;
	channel.txTimer->Cancel();
	channel.pollTimer->Cancel();
}


void
Escc::SetBackend(int index, SerialBackend* backend)
{
	fChannels[index].backend = backend;
	_UpdateLineConfig(index);
	_SchedulePoll(index);
}


u64
Escc::_CharacterTicks(int index) const
{
	const Channel& channel = fChannels[index];
	u32 constant = channel.wr[12] | (channel.wr[13] << 8);
	static const int kClockModes[] = { 1, 16, 32, 64 };
	int clockMode = kClockModes[channel.wr[4] >> 6];
	u64 baud = kClockHz / (2 * clockMode * (u64)(constant + 2));
	if (_Synchronous(index)) {
		// 8 bits a character, no start and stop bits
		baud = std::max<u64>(300, std::min<u64>(baud, 1000000));
		return fTicksPerSecond * 8 / baud;
	}
	baud = std::max<u64>(300, std::min<u64>(baud, 230400));
	return fTicksPerSecond * 10 / baud;
}


void
Escc::_UpdateLineConfig(int index)
{
	Channel& channel = fChannels[index];
	if (channel.backend == nullptr)
		return;
	SerialLineConfig config;
	u32 constant = channel.wr[12] | (channel.wr[13] << 8);
	static const int kClockModes[] = { 1, 16, 32, 64 };
	int clockMode = kClockModes[channel.wr[4] >> 6];
	config.baud = kClockHz / (2 * clockMode * (constant + 2));
	static const int kBits[] = { 5, 7, 6, 8 };
	config.dataBits = kBits[channel.wr[5] >> 5 & 3];
	config.parity = (channel.wr[4] & 1) == 0 ? 'N'
		: (channel.wr[4] & 2) != 0 ? 'E' : 'O';
	config.stopBits = (channel.wr[4] & 0x0c) == 0x0c ? 2 : 1;
	channel.backend->SetLineConfig(config);
	channel.backend->SetModemControl((channel.wr[5] & 0x80) != 0,
		(channel.wr[5] & 0x02) != 0);
}


void
Escc::_StartTransmitter(int index)
{
	Channel& channel = fChannels[index];
	if (channel.txBusy || !channel.txFull)
		return;
	channel.txShift = channel.txBuffer;
	channel.txFull = false;
	channel.txBusy = true;
	// the buffer is free again: a transmit interrupt if enabled
	if ((channel.wr[1] & 0x02) != 0)
		channel.txInterruptPending = true;
	channel.txTimer->ArmIn(_CharacterTicks(index));
	_UpdateInterrupt();
}


void
Escc::_TransmitDone(int index)
{
	Channel& channel = fChannels[index];
	channel.txBusy = false;
	if (_Synchronous(index)) {
		// a frame on the idle network: nobody receives it. With nothing
		// more to send it ends (CRC and closing flag): the latch is set.
		_StartTransmitter(index);
		if (!channel.txBusy)
			channel.txUnderrun = true;
		_CheckStatus(index);
		_UpdateInterrupt();
		return;
	}
	if ((channel.wr[14] & 0x10) != 0) {
		// local loopback
		if (channel.rx.size() < kRxFifoSize)
			channel.rx.push_back(channel.txShift);
	} else if (channel.backend != nullptr)
		channel.backend->Transmit(channel.txShift);
	_StartTransmitter(index);
	_UpdateInterrupt();
}


void
Escc::_SchedulePoll(int index)
{
	Channel& channel = fChannels[index];
	if (channel.backend == nullptr || (channel.wr[3] & 0x01) == 0) {
		channel.pollTimer->Cancel();
		return;
	}
	if (!channel.pollTimer->IsArmed())
		channel.pollTimer->ArmIn(_CharacterTicks(index));
}


void
Escc::_Poll(int index)
{
	Channel& channel = fChannels[index];
	if (channel.backend == nullptr || (channel.wr[3] & 0x01) == 0
		|| _Synchronous(index))
		return;
	if (channel.rx.size() < kRxFifoSize) {
		int byte = channel.backend->Receive();
		if (byte >= 0) {
			channel.rx.push_back((u8)byte);
			u8 mode = (channel.wr[1] >> 3) & 3;
			if (mode == 1 || mode == 2)
				channel.rxInterruptPending = true;
			_UpdateInterrupt();
		}
	}
	// poll quickly while data comes, else every 2 ms
	u64 interval = _CharacterTicks(index);
	if (channel.rx.empty())
		interval = std::max(interval, fTicksPerSecond / 500);
	channel.pollTimer->ArmIn(interval);
}


void
Escc::_UpdateInterrupt()
{
	bool line = false;
	if ((fChannels[0].wr[9] & 0x08) != 0) {
		for (const Channel& channel : fChannels) {
			if (channel.txInterruptPending || channel.rxInterruptPending
				|| channel.extInterruptPending)
				line = true;
		}
	}
	if (line == fInterruptLine)
		return;
	fInterruptLine = line;
	if (fInterrupt)
		fInterrupt(line);
}


u8
Escc::_Status0(int index)
{
	Channel& channel = fChannels[index];
	u8 value = kRr0Dcd | kRr0Cts;
	if (!channel.rx.empty())
		value |= kRr0RxAvailable;
	if (!channel.txFull)
		value |= kRr0TxEmpty;
	if (channel.backend != nullptr && !_Synchronous(index)) {
		SerialModemStatus status = channel.backend->ModemStatus();
		if (!status.dcd)
			value &= ~kRr0Dcd;
		if (!status.cts)
			value &= ~kRr0Cts;
	}
	// the receiver hunts for flags (none come); while it is disabled too
	if (_Synchronous(index))
		value |= kRr0SyncHunt;
	if (channel.txUnderrun)
		value |= kRr0TxUnderrun;
	return value;
}


void
Escc::_CheckStatus(int index)
{
	Channel& channel = fChannels[index];
	u8 status = _Status0(index);
	u8 changed = (status ^ channel.lastStatus) & channel.wr[15] & 0xf8;
	channel.lastStatus = status;
	if (changed != 0 && (channel.wr[1] & 0x01) != 0)
		channel.extInterruptPending = true;
}


u8
Escc::_ReadRegister(int index, int reg)
{
	Channel& channel = fChannels[index];
	switch (reg) {
		case 0:
		case 4:
			return _Status0(index);
		case 1:
		case 5:
			// all sent; residue code 011 (async: 8 bits)
			return (channel.txBusy || channel.txFull ? 0 : 0x01)
				| 0x06 | (channel.rxOverrun ? 0x20 : 0);
		case 2:
		case 6:
			// the vector (channel B: modified by the status, not done)
			return fChannels[0].wr[2];
		case 3:
		case 7:
		{
			if (index != 0)
				return 0;
			u8 value = 0;
			if (fChannels[1].extInterruptPending)
				value |= kIpBExternal;
			if (fChannels[1].txInterruptPending)
				value |= kIpBTx;
			if (fChannels[1].rxInterruptPending)
				value |= kIpBRx;
			if (fChannels[0].extInterruptPending)
				value |= kIpAExternal;
			if (fChannels[0].txInterruptPending)
				value |= kIpATx;
			if (fChannels[0].rxInterruptPending)
				value |= kIpARx;
			return value;
		}
		case 8:
			return ReadData(index);
		case 10:
		case 14:
			return 0;
		case 12:
			return channel.wr[12];
		case 13:
		case 9:
			return channel.wr[13];
		case 15:
		case 11:
			return channel.wr[15];
	}
	return 0;
}


void
Escc::_WriteRegister(int index, int reg, u8 value)
{
	Channel& channel = fChannels[index];
	switch (reg) {
		case 0:
		{
			channel.pointer = value & 7;
			if ((value & 0x38) == 0x08)
				channel.pointer |= 8;		// point high
			switch (value & 0x38) {
				case 0x10:		// reset external/status interrupts
					channel.extInterruptPending = false;
					channel.lastStatus = _Status0(index);
					break;
				case 0x28:		// reset transmit interrupt pending
					channel.txInterruptPending = false;
					break;
				case 0x30:		// error reset
					channel.rxOverrun = false;
					break;
				case 0x38:		// reset highest interrupt under service
					break;
			}
			if ((value & 0xc0) == 0xc0)		// reset Tx underrun/EOM latch
				channel.txUnderrun = false;
			_CheckStatus(index);
			_UpdateInterrupt();
			return;
		}
		case 2:
			// shared by both channels
			fChannels[0].wr[2] = fChannels[1].wr[2] = value;
			return;
		case 8:
			WriteData(index, value);
			return;
		case 9:
		{
			// shared; bits 7-6 reset a channel or the chip
			switch (value & 0xc0) {
				case 0x40:
					_ResetChannel(1, false);
					break;
				case 0x80:
					_ResetChannel(0, false);
					break;
				case 0xc0:
					_ResetChannel(0, true);
					_ResetChannel(1, true);
					break;
			}
			fChannels[0].wr[9] = fChannels[1].wr[9] = value & 0x3f;
			_UpdateInterrupt();
			return;
		}
	}
	channel.wr[reg] = value;
	switch (reg) {
		case 1:
			if ((value & 0x02) == 0)
				channel.txInterruptPending = false;
			if ((value & 0x18) == 0)
				channel.rxInterruptPending = false;
			_UpdateInterrupt();
			break;
		case 3:
			_SchedulePoll(index);
			_CheckStatus(index);
			_UpdateInterrupt();
			break;
		case 4:
			_UpdateLineConfig(index);
			_CheckStatus(index);
			_UpdateInterrupt();
			break;
		case 5:
		case 12:
		case 13:
			_UpdateLineConfig(index);
			break;
		case 15:
			// changes from now on interrupt
			channel.lastStatus = _Status0(index);
			break;
	}
}


u8
Escc::ReadControl(int index)
{
	Channel& channel = fChannels[index];
	int reg = channel.pointer;
	channel.pointer = 0;
	u8 value = _ReadRegister(index, reg);
	PM_LOG(kLogSerial, kLogTrace, "escc %c: RR%d = %02x", 'A' + index, reg,
		value);
	return value;
}


void
Escc::WriteControl(int index, u8 value)
{
	Channel& channel = fChannels[index];
	int reg = channel.pointer;
	channel.pointer = 0;
	if (reg != 0 || (value & 0xf8) != 0) {
		PM_LOG(kLogSerial, kLogTrace, "escc %c: WR%d = %02x", 'A' + index,
			reg, value);
	}
	_WriteRegister(index, reg, value);
}


u8
Escc::ReadData(int index)
{
	Channel& channel = fChannels[index];
	if (channel.rx.empty())
		return 0;
	u8 value = channel.rx.front();
	channel.rx.pop_front();
	if (channel.rx.empty())
		channel.rxInterruptPending = false;
	_UpdateInterrupt();
	_SchedulePoll(index);
	return value;
}


void
Escc::WriteData(int index, u8 value)
{
	PM_LOG(kLogSerial, kLogTrace, "escc %c: data out %02x", 'A' + index,
		value);
	Channel& channel = fChannels[index];
	channel.txBuffer = value;
	channel.txFull = true;
	channel.txInterruptPending = false;
	_StartTransmitter(index);
	_UpdateInterrupt();
}


u32
Escc::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = addr & 0x1fff;
	int channel;
	bool data;
	if (offset >= 0x1000) {
		// MacRISC: B at 0x00, A at 0x20; data 0x10 above control
		offset -= 0x1000;
		if (offset >= 0x40)
			return 0;
		channel = offset >= 0x20 ? 0 : 1;
		data = (offset & 0x10) != 0;
	} else {
		if (offset >= 8)
			return 0;
		channel = (offset & 2) != 0 ? 0 : 1;
		data = (offset & 4) != 0;
	}
	u8 value = data ? ReadData(channel) : ReadControl(channel);
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | value;
	return result;
}


void
Escc::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr & 0x1fff;
	u8 byte = (u8)(value >> (8 * (size - 1)));
	int channel;
	bool data;
	if (offset >= 0x1000) {
		offset -= 0x1000;
		if (offset >= 0x40)
			return;
		channel = offset >= 0x20 ? 0 : 1;
		data = (offset & 0x10) != 0;
	} else {
		if (offset >= 8)
			return;
		channel = (offset & 2) != 0 ? 0 : 1;
		data = (offset & 4) != 0;
	}
	if (data)
		WriteData(channel, byte);
	else
		WriteControl(channel, byte);
}

}	// namespace powermac
