/*
 * PowerMac emulator - AWACS, the 9500's sound chip (Audio Waveform
 * Amplifier and Converter for Sound).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * In Grand Central at 0x14000; sound out through DBDMA channel 8, in
 * through channel 9, its own interrupt is Grand Central's 17. Registers,
 * 32 bits little endian, every 16 bytes (after the register descriptions
 * in NetBSD's awacs.c and Linux's awacs.h):
 *   0x00 sound control: bits 3-0 input subframe, 7-4 output subframe,
 *        10-8 the sample rate (0 44100, 1 29400, 2 22050, 3 17640, 4
 *        14700, 5 11025, 6 8820, 7 7350 Hz), 11 error, 12 port change,
 *        13/14 their interrupt enables
 *   0x10 codec control: a codec register write, (address << 12) | 12 bits
 *        of data; bit 24 stays set while it is sent (done at once here)
 *   0x20 codec status: bit 22 valid, bits 15-12 revision, 11-8
 *        manufacturer, 3-0 the jacks (headphones 8, line out 4, line in 2,
 *        microphone 1)
 *   0x30 clipping count (read clears)   0x40 byte swapping
 * Codec registers: 1 bit 7 mutes the speaker, bit 9 the headphones; 2 the
 * headphone and 4 the speaker attenuation (left bits 9-6, right 3-0, 1.5
 * dB a step).
 *
 * The output takes 16-bit big endian stereo frames (little endian with
 * byte swapping on) from DMA into a FIFO, and plays them at the sample
 * rate in emulated time.
 */
#ifndef PM_AWACS_H
#define PM_AWACS_H

#include "Dbdma.h"
#include "../board/Scheduler.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace powermac {

class AudioSink;

class Awacs : public MmioDevice {
public:
								Awacs(Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Awacs();

			void				Reset();
			void				SetOutputDma(DbdmaChannel* channel);
			void				SetInputDma(DbdmaChannel* channel);
			void				SetSink(AudioSink* sink) { fSink = sink; }
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }

			u32					SampleRate() const;
			// Frames played since power on (tests).
			u64					FramesPlayed() const { return fPlayed; }

	// MmioDevice: offsets within the chip's 4 KB
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "awacs"; }

private:
	// DMA ends: the output takes frames into the FIFO, the input hands
	// out silence at the sample rate.
	class Output : public DbdmaChannel::Device {
	public:
		Output(Awacs& awacs) : fAwacs(awacs) {}
		virtual u32 DmaOutput(const u8* data, u32 length);
	private:
		Awacs& fAwacs;
	};
	class Input : public DbdmaChannel::Device {
	public:
		Input(Awacs& awacs) : fAwacs(awacs) {}
		virtual u32 DmaInput(u8* data, u32 length);
	private:
		Awacs& fAwacs;
	};

			u32					_Read32(u32 offset) const;
			void				_Write32(u32 offset, u32 value);
			void				_Tick();
			float				_Gain() const;

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			AudioSink*			fSink;
			std::function<void(bool)> fInterrupt;
			Output				fOutput;
			Input				fInput;
			DbdmaChannel*		fOutputDma;
			DbdmaChannel*		fInputDma;

			u32					fControl;
			u32					fCodecControl;
			u16					fCodec[8];
			u32					fByteSwap;
			std::deque<u8>		fFifo;			// output bytes from DMA
			u32					fInputDue;		// input bytes to hand out
			u64					fLastTick;
			u64					fFraction;		// frames * ticks, carried
			u64					fPlayed;
			std::vector<s16>	fFrames;
			std::unique_ptr<Timer> fTimer;
};

}	// namespace powermac

#endif	// PM_AWACS_H
