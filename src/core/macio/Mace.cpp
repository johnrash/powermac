/*
 * PowerMac emulator - MACE, the AMD Am79C940 Ethernet controller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Mace.h"

#include "../host/NetworkBackend.h"
#include "../Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace powermac {

namespace {

enum {
	kRegReceiveFifo = 0, kRegTransmitFifo, kRegTransmitControl,
	kRegTransmitStatus, kRegTransmitRetries, kRegReceiveControl,
	kRegReceiveStatus, kRegFifoCount, kRegInterrupt, kRegInterruptMask,
	kRegPoll, kRegBiuControl, kRegFifoControl, kRegMacControl,
	kRegPlsControl, kRegPhyControl, kRegChipIdLow, kRegChipIdHigh,
	kRegAddressControl, kRegReserved19, kRegFilter, kRegAddress,
	kRegMissed = 24, kRegRunts = 26, kRegCollisions = 27, kRegUserTest = 29
};

enum {
	kAutoPad = 0x01,			// XMTFC
	kDisableFcs = 0x08,
	kStatusValid = 0x80,		// XMTFS, PR
	kTransmitRequest = 0x40,	// PR
	kReceiveRequest = 0x20,
	kAutoStrip = 0x01,			// RCVFC
	kIntMissedOverflow = 0x04,	// IR
	kIntReceive = 0x02,
	kIntTransmit = 0x01,
	kSoftwareReset = 0x01,		// BIUCC
	kPromiscuous = 0x80,		// MACCC
	kNoPhysical = 0x08,
	kNoBroadcast = 0x04,
	kEnableTransmit = 0x02,
	kEnableReceive = 0x01,
	kPortSelect = 0x06,			// PLSCC
	kPort10BaseT = 0x02,
	kLinkFail = 0x80,			// PHYCC
	kNoLinkTest = 0x40,
	kAddressChange = 0x80,		// IAC
	kPhysicalReset = 0x04,
	kLogicalReset = 0x02,
	kLoopback = 0x06			// UTR
};

// Grand Central's DMA status bits for the MACE: the transmit channel's S5
// is set when the frame has gone (Apple's driver waits for it after each
// OUTPUT_LAST), the receive channel's S6 marks the descriptor that ends a
// frame (the drivers look for it in the written back status).
const u8 kTransmitDone = 0x20;
const u8 kReceiveEnd = 0x40;

const u16 kChipId = 0x0940;
const size_t kMaxFrame = 1536;
const size_t kMinFrame = 60;	// without the FCS
const size_t kReceiveFrames = 8;


// The Ethernet CRC: reflected, polynomial 0x04C11DB7.
u32
Crc32(const u8* data, size_t length)
{
	u32 crc = 0xffffffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
	}
	return crc;
}


// The frame in hex, 16 bytes a line, at the trace level.
void
DumpFrame(const char* what, const u8* data, size_t length)
{
	if (!Log::Enabled(kLogNetwork, kLogTrace))
		return;
	for (size_t line = 0; line < length; line += 16) {
		char text[16 * 3 + 1];
		size_t used = 0;
		for (size_t i = line; i < length && i < line + 16; i++)
			used += snprintf(text + used, sizeof(text) - used, " %02x",
				data[i]);
		text[used] = '\0';
		PM_LOG(kLogNetwork, kLogTrace, "mace: %s %04zx:%s", what, line, text);
	}
}

}	// namespace


Mace::Mace(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fBackend(nullptr),
	fTransmitDma(nullptr),
	fReceiveDma(nullptr),
	fTransmitter(*this),
	fReceiver(*this),
	fFramesSent(0),
	fFramesReceived(0)
{
	fTransmitTimer.reset(new Timer(scheduler, [this]() { _TransmitDone(); },
		"mace-transmit"));
	fPollTimer.reset(new Timer(scheduler, [this]() { _Poll(); },
		"mace-poll"));
	Reset();
}


Mace::~Mace()
{
}


void
Mace::Reset()
{
	fTransmitControl = kAutoPad;
	fTransmitStatus = 0;
	fReceiveControl = kAutoStrip;
	fInterruptStatus = 0;
	fInterruptMask = 0;
	fBiuControl = 0;
	fFifoControl = 0;
	fMacControl = 0;
	fPlsControl = 0;
	fPhyControl = 0;
	fAddressControl = 0;
	fUserTest = 0;
	memset(fAddress, 0, sizeof(fAddress));
	memset(fFilter, 0, sizeof(fFilter));
	fAddressIndex = 0;
	fFilterIndex = 0;
	fMissed = 0;
	memset(fReceiveStatus, 0, sizeof(fReceiveStatus));
	fReceiveStatusIndex = 0;
	fFrame.clear();
	fSending = false;
	fStatusPending = false;
	fReceived.clear();
	fReceivePosition = 0;
	fTransmitTimer->Cancel();
	_UpdateInterrupt();
}


void
Mace::SetTransmitDma(DbdmaChannel* channel)
{
	fTransmitDma = channel;
	channel->SetDevice(&fTransmitter);
}


void
Mace::SetReceiveDma(DbdmaChannel* channel)
{
	fReceiveDma = channel;
	channel->SetDevice(&fReceiver);
}


void
Mace::SetBackend(NetworkBackend* backend)
{
	fBackend = backend;
	if (backend == nullptr) {
		fPollTimer->Cancel();
		return;
	}
	backend->SetReceiver([this](const u8* frame, size_t length) {
		return Receive(frame, length);
	});
	fPollTimer->ArmIn(fTicksPerSecond / 1000);
}


// The host side is serviced every millisecond of emulated time.
void
Mace::_Poll()
{
	if (fBackend == nullptr)
		return;
	fBackend->Poll();
	fPollTimer->ArmIn(fTicksPerSecond / 1000);
}


// #pragma mark - transmit


bool
Mace::_TransmitReady() const
{
	return (fMacControl & kEnableTransmit) != 0 && !fSending
		&& !fStatusPending;
}


u32
Mace::Transmitter::DmaOutput(const u8* data, u32 length)
{
	if (!fMace._TransmitReady())
		return 0;
	if (fMace.fFrame.empty())
		fMace.fTransmitDma->SetDeviceStatus(kTransmitDone, 0);
	size_t room = kMaxFrame - std::min(kMaxFrame, fMace.fFrame.size());
	fMace.fFrame.insert(fMace.fFrame.end(), data,
		data + std::min<size_t>(length, room));
	return length;
}


void
Mace::Transmitter::DmaOutputLast()
{
	if (!fMace.fFrame.empty())
		fMace._EndFrame();
}


void
Mace::_EndFrame()
{
	std::vector<u8> frame;
	frame.swap(fFrame);
	if ((fTransmitControl & kDisableFcs) != 0 && frame.size() >= 4)
		frame.resize(frame.size() - 4);
	if (frame.size() < kMinFrame && (fTransmitControl & kAutoPad) != 0)
		frame.resize(kMinFrame, 0);
	PM_LOG(kLogNetwork, kLogDebug, "mace: send %zu bytes to "
		"%02x:%02x:%02x:%02x:%02x:%02x type %02x%02x", frame.size(),
		frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
		frame.size() >= 14 ? frame[12] : 0, frame.size() >= 14 ? frame[13] : 0);
	DumpFrame("sent", frame.data(), frame.size());
	fFramesSent++;
	if ((fUserTest & kLoopback) != 0)
		Receive(frame.data(), frame.size());
	else if (fBackend != nullptr)
		fBackend->Send(frame.data(), frame.size());
	// on the wire: preamble, the frame, its FCS and the gap after it, at
	// 10 Mbit/s
	fSending = true;
	u64 bits = (u64)(8 + frame.size() + 4 + 12) * 8;
	fTransmitTimer->ArmIn(bits * fTicksPerSecond / 10000000);
}


void
Mace::_TransmitDone()
{
	fSending = false;
	fStatusPending = true;
	fTransmitStatus = kStatusValid;
	fInterruptStatus |= kIntTransmit;
	_UpdateInterrupt();
	if (fTransmitDma != nullptr)
		fTransmitDma->SetDeviceStatus(kTransmitDone, kTransmitDone);
}


// #pragma mark - receive


bool
Mace::_Accepts(const u8* destination) const
{
	if ((fMacControl & kPromiscuous) != 0)
		return true;
	if ((destination[0] & 1) == 0) {
		return (fMacControl & kNoPhysical) == 0
			&& memcmp(destination, fAddress, 6) == 0;
	}
	static const u8 kBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	if (memcmp(destination, kBroadcast, 6) == 0)
		return (fMacControl & kNoBroadcast) == 0;
	// multicast: the top six bits of the address's CRC (not inverted)
	// pick a bit of the logical address filter
	u32 crc = Crc32(destination, 6);
	int index = crc >> 26;
	return (fFilter[index >> 3] & (1 << (index & 7))) != 0;
}


void
Mace::_Missed()
{
	if (++fMissed == 0) {
		fInterruptStatus |= kIntMissedOverflow;
		_UpdateInterrupt();
	}
}


bool
Mace::Receive(const u8* frame, size_t length)
{
	if ((fMacControl & kEnableReceive) == 0 || length < 14
		|| length > kMaxFrame) {
		PM_LOG(kLogNetwork, kLogDebug, "mace: %zu bytes lost (receiver "
			"off)", length);
		return true;
	}
	if (!_Accepts(frame)) {
		PM_LOG(kLogNetwork, kLogDebug, "mace: %zu bytes for "
			"%02x:%02x:%02x:%02x:%02x:%02x filtered out", length, frame[0],
			frame[1], frame[2], frame[3], frame[4], frame[5]);
		return true;
	}
	if (fReceived.size() >= kReceiveFrames)
		return false;		// waits in the backend
	std::vector<u8> data(frame, frame + length);
	if (data.size() < kMinFrame)
		data.resize(kMinFrame, 0);
	u16 type = (u16)(data[12] << 8 | data[13]);
	if ((fReceiveControl & kAutoStrip) != 0 && type <= 1500) {
		// 802.3: the pad and the FCS are stripped
		data.resize(std::min(data.size(), (size_t)14 + type));
	} else {
		u32 fcs = ~Crc32(data.data(), data.size());
		for (int i = 0; i < 4; i++)
			data.push_back((u8)(fcs >> (8 * i)));
	}
	size_t count = data.size();
	data.push_back((u8)count);
	data.push_back((u8)((count >> 8) & 0x0f));
	data.push_back(0);		// runt packets
	data.push_back(0);		// collisions
	PM_LOG(kLogNetwork, kLogDebug, "mace: receive %zu bytes type %04x",
		length, type);
	DumpFrame("received", frame, length);
	fReceived.push_back(std::move(data));
	fFramesReceived++;
	if (fReceiveDma != nullptr)
		fReceiveDma->Kick();
	return true;
}


u32
Mace::Receiver::DmaInput(u8* data, u32 length)
{
	if (fMace.fReceived.empty())
		return 0;
	std::vector<u8>& frame = fMace.fReceived.front();
	if (fMace.fReceivePosition == 0)
		fMace.fReceiveDma->SetDeviceStatus(kReceiveEnd, 0);
	u32 given = (u32)std::min<size_t>(length,
		frame.size() - fMace.fReceivePosition);
	memcpy(data, frame.data() + fMace.fReceivePosition, given);
	fMace.fReceivePosition += given;
	if (fMace.fReceivePosition == frame.size()) {
		memcpy(fMace.fReceiveStatus, frame.data() + frame.size() - 4, 4);
		fMace.fReceived.pop_front();
		fMace.fReceivePosition = 0;
		fMace.fInterruptStatus |= kIntReceive;
		fMace._UpdateInterrupt();
		// the end of the frame ends the descriptor
		fMace.fReceiveDma->SetDeviceStatus(kReceiveEnd, kReceiveEnd);
		fMace.fReceiveDma->EndInput();
	}
	return given;
}


// #pragma mark - registers


void
Mace::_UpdateInterrupt()
{
	if (fInterrupt)
		fInterrupt((fInterruptStatus & ~fInterruptMask) != 0);
}


u32
Mace::MmioRead(u32 addr, int size, int initiator)
{
	int reg = (addr >> 4) & 0x1f;
	u8 value = 0;
	switch (reg) {
		case kRegReceiveFifo:
			PM_LOG(kLogNetwork, kLogInfo, "mace: programmed I/O receive "
				"not emulated");
			break;
		case kRegTransmitControl:
			value = fTransmitControl;
			break;
		case kRegTransmitStatus:
			value = fTransmitStatus;
			if ((fTransmitStatus & kStatusValid) != 0) {
				fTransmitStatus = 0;
				fStatusPending = false;
				if (fTransmitDma != nullptr && _TransmitReady())
					fTransmitDma->Kick();
			}
			break;
		case kRegTransmitRetries:
			break;
		case kRegReceiveControl:
			value = fReceiveControl;
			break;
		case kRegReceiveStatus:
			value = fReceiveStatus[fReceiveStatusIndex];
			fReceiveStatusIndex = (fReceiveStatusIndex + 1) & 3;
			break;
		case kRegFifoCount:
			value = (u8)(std::min<size_t>(fReceived.size(), 15) << 4
				| (fSending || fStatusPending ? 1 : 0));
			break;
		case kRegInterrupt:
			value = fInterruptStatus;
			fInterruptStatus = 0;
			_UpdateInterrupt();
			break;
		case kRegInterruptMask:
			value = fInterruptMask;
			break;
		case kRegPoll:
			value = (fStatusPending ? kStatusValid : 0)
				| (_TransmitReady() ? kTransmitRequest : 0)
				| (!fReceived.empty() ? kReceiveRequest : 0);
			break;
		case kRegBiuControl:
			value = fBiuControl;
			break;
		case kRegFifoControl:
			value = fFifoControl;
			break;
		case kRegMacControl:
			value = fMacControl;
			break;
		case kRegPlsControl:
			value = fPlsControl;
			break;
		case kRegPhyControl:
			value = fPhyControl;
			if (fBackend == nullptr && (fPhyControl & kNoLinkTest) == 0
				&& (fPlsControl & kPortSelect) == kPort10BaseT)
				value |= kLinkFail;
			break;
		case kRegChipIdLow:
			value = (u8)kChipId;
			break;
		case kRegChipIdHigh:
			value = (u8)(kChipId >> 8);
			break;
		case kRegAddressControl:
			value = fAddressControl;
			break;
		case kRegFilter:
			value = fFilter[fFilterIndex];
			fFilterIndex = (fFilterIndex + 1) & 7;
			break;
		case kRegAddress:
			value = fAddress[fAddressIndex];
			fAddressIndex = (fAddressIndex + 1) % 6;
			break;
		case kRegMissed:
			value = fMissed;
			fMissed = 0;
			break;
		case kRegUserTest:
			value = fUserTest;
			break;
		default:
			break;
	}
	PM_LOG(kLogNetwork, kLogTrace, "mace: read %d = %02x", reg, value);
	return value;
}


void
Mace::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	int reg = (addr >> 4) & 0x1f;
	u8 byte = (u8)value;
	PM_LOG(kLogNetwork, kLogTrace, "mace: write %d = %02x", reg, byte);
	switch (reg) {
		case kRegTransmitFifo:
			PM_LOG(kLogNetwork, kLogInfo, "mace: programmed I/O transmit "
				"not emulated");
			break;
		case kRegTransmitControl:
			fTransmitControl = byte;
			break;
		case kRegReceiveControl:
			fReceiveControl = byte;
			break;
		case kRegInterruptMask:
			fInterruptMask = byte;
			_UpdateInterrupt();
			break;
		case kRegBiuControl:
			if ((byte & kSoftwareReset) != 0) {
				Reset();
				break;
			}
			fBiuControl = byte;
			break;
		case kRegFifoControl:
			fFifoControl = byte;
			break;
		case kRegMacControl:
		{
			bool wasReady = _TransmitReady();
			fMacControl = byte;
			if (!wasReady && _TransmitReady() && fTransmitDma != nullptr)
				fTransmitDma->Kick();
			break;
		}
		case kRegPlsControl:
			fPlsControl = byte;
			break;
		case kRegPhyControl:
			fPhyControl = byte;
			break;
		case kRegAddressControl:
			// the address can change at once: ADDRCHG reads back clear
			fAddressControl = byte & ~(kAddressChange | kPhysicalReset
				| kLogicalReset);
			if ((byte & kPhysicalReset) != 0)
				fAddressIndex = 0;
			if ((byte & kLogicalReset) != 0)
				fFilterIndex = 0;
			break;
		case kRegFilter:
			fFilter[fFilterIndex] = byte;
			fFilterIndex = (fFilterIndex + 1) & 7;
			break;
		case kRegAddress:
			fAddress[fAddressIndex] = byte;
			fAddressIndex = (fAddressIndex + 1) % 6;
			if (fAddressIndex == 0) {
				PM_LOG(kLogNetwork, kLogInfo, "mace: address "
					"%02x:%02x:%02x:%02x:%02x:%02x", fAddress[0],
					fAddress[1], fAddress[2], fAddress[3], fAddress[4],
					fAddress[5]);
			}
			break;
		case kRegUserTest:
			fUserTest = byte;
			break;
		default:
			break;
	}
}

}	// namespace powermac
