/*
 * PowerMac emulator - AWACS, the 9500's sound chip.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Awacs.h"

#include "../Log.h"
#include "../host/AudioSink.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace powermac {

namespace {

enum {
	kRegControl = 0x00,
	kRegCodecControl = 0x10,
	kRegCodecStatus = 0x20,
	kRegClipCount = 0x30,
	kRegByteSwap = 0x40
};

enum {
	kControlRateShift = 8,
	kControlError = 0x0800,
	kControlPortChange = 0x1000,
	kControlErrorInterrupt = 0x2000,
	kControlPortChangeInterrupt = 0x4000,
	kCodecBusy = 0x01000000,
	kStatusValid = 0x00400000,
	kStatusRevision = 2 << 12,
	kStatusManufacturer = 1 << 8,
	kMuteSpeaker = 0x0080,
	kMuteHeadphones = 0x0200
};

const u32 kRates[8] = { 44100, 29400, 22050, 17640, 14700, 11025, 8820,
	7350 };
// The output FIFO: about 12 ms at 44.1 kHz.
const size_t kFifoBytes = 2048;

}	// namespace


Awacs::Awacs(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fSink(nullptr),
	fOutput(*this),
	fInput(*this),
	fOutputDma(nullptr),
	fInputDma(nullptr)
{
	fTimer.reset(new Timer(scheduler, [this]() { _Tick(); }, "awacs"));
	Reset();
}


Awacs::~Awacs()
{
	if (fOutputDma != nullptr)
		fOutputDma->SetDevice(nullptr);
	if (fInputDma != nullptr)
		fInputDma->SetDevice(nullptr);
}


void
Awacs::Reset()
{
	fControl = 0;
	fCodecControl = 0;
	memset(fCodec, 0, sizeof(fCodec));
	fByteSwap = 0;
	fFifo.clear();
	fInputDue = 0;
	fLastTick = fScheduler.Now();
	fFraction = 0;
	fPlayed = 0;
	fTimer->Cancel();
	fTimer->ArmIn(fTicksPerSecond / 1000);
}


void
Awacs::SetOutputDma(DbdmaChannel* channel)
{
	fOutputDma = channel;
	if (channel != nullptr)
		channel->SetDevice(&fOutput);
}


void
Awacs::SetInputDma(DbdmaChannel* channel)
{
	fInputDma = channel;
	if (channel != nullptr)
		channel->SetDevice(&fInput);
}


u32
Awacs::SampleRate() const
{
	return kRates[(fControl >> kControlRateShift) & 7];
}


u32
Awacs::Output::DmaOutput(const u8* data, u32 length)
{
	size_t space = kFifoBytes - fAwacs.fFifo.size();
	u32 taken = (u32)std::min<size_t>(space, length);
	fAwacs.fFifo.insert(fAwacs.fFifo.end(), data, data + taken);
	return taken;
}


u32
Awacs::Input::DmaInput(u8* data, u32 length)
{
	// no microphone: silence, as fast as the sample rate gives it
	u32 given = std::min(length, fAwacs.fInputDue);
	memset(data, 0, given);
	fAwacs.fInputDue -= given;
	return given;
}


// The speaker's gain: 1.5 dB less per attenuation step, nothing when muted
// (headphones are never plugged in).
float
Awacs::_Gain() const
{
	if ((fCodec[1] & kMuteSpeaker) != 0)
		return 0;
	int attenuation = std::max(fCodec[4] & 0x0f, (fCodec[4] >> 6) & 0x0f);
	return powf(10, -1.5f * attenuation / 20);
}


// Every millisecond: the frames of the elapsed time go to the sink.
void
Awacs::_Tick()
{
	fTimer->ArmIn(fTicksPerSecond / 1000);
	u64 now = fScheduler.Now();
	u64 elapsed = now - fLastTick;
	fLastTick = now;
	u32 rate = SampleRate();
	fFraction += elapsed * rate;
	u32 frames = (u32)(fFraction / fTicksPerSecond);
	fFraction %= fTicksPerSecond;

	bool outputRunning = fOutputDma != nullptr && fOutputDma->IsRunning();
	u32 available = (u32)(fFifo.size() / 4);
	u32 count = std::min(frames, available);
	if (count > 0) {
		fFrames.resize((size_t)count * 2);
		bool swap = fByteSwap != 0;
		float gain = _Gain();
		for (u32 i = 0; i < count * 2; i++) {
			u8 first = fFifo.front();
			fFifo.pop_front();
			u8 second = fFifo.front();
			fFifo.pop_front();
			s16 sample = (s16)(swap ? (second << 8) | first
				: (first << 8) | second);
			fFrames[i] = gain == 1 ? sample : (s16)(sample * gain);
		}
		fPlayed += count;
		if (fSink != nullptr)
			fSink->Play(fFrames.data(), count, rate);
	}
	if (outputRunning)
		fOutputDma->Kick();

	if (fInputDma != nullptr && fInputDma->IsRunning()) {
		fInputDue = std::min<u32>(fInputDue + frames * 4, rate);
		fInputDma->Kick();
	} else
		fInputDue = 0;
}


u32
Awacs::_Read32(u32 offset) const
{
	switch (offset) {
		case kRegControl:
			return fControl;
		case kRegCodecControl:
			return fCodecControl & ~kCodecBusy;
		case kRegCodecStatus:
			// nothing plugged into the jacks
			return kStatusValid | kStatusRevision | kStatusManufacturer;
		case kRegClipCount:
			return 0;
		case kRegByteSwap:
			return fByteSwap;
	}
	return 0;
}


void
Awacs::_Write32(u32 offset, u32 value)
{
	switch (offset) {
		case kRegControl:
		{
			u32 oldRate = SampleRate();
			// the status bits clear when written with ones
			u32 status = fControl & (kControlError | kControlPortChange);
			status &= ~(value & (kControlError | kControlPortChange));
			fControl = (value & ~(kControlError | kControlPortChange))
				| status;
			if (SampleRate() != oldRate) {
				PM_LOG(kLogAudio, kLogDebug, "awacs: %u Hz", SampleRate());
			}
			break;
		}
		case kRegCodecControl:
		{
			fCodecControl = value;
			int reg = (value >> 12) & 7;
			fCodec[reg] = value & 0xfff;
			PM_LOG(kLogAudio, kLogDebug, "awacs: codec register %d = %03x",
				reg, value & 0xfff);
			break;
		}
		case kRegByteSwap:
			fByteSwap = value;
			break;
	}
}


u32
Awacs::MmioRead(u32 addr, int size, int initiator)
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
Awacs::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr & 0xff;
	u32 reg = offset & ~3u;
	u8 bytes[4];
	StoreLE32(bytes, _Read32(reg));
	for (int i = 0; i < size; i++)
		bytes[(offset + i) & 3] = (u8)(value >> (8 * (size - 1 - i)));
	_Write32(reg, LoadLE32(bytes));
}

}	// namespace powermac
