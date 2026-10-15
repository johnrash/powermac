/*
 * PowerMac emulator - host side of the emulated sound output.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Play() is called from the emulation thread with stereo 16-bit frames at
 * the codec's sample rate, in emulated real time; the host buffers and
 * resamples them for its own audio output.
 */
#ifndef PM_AUDIO_SINK_H
#define PM_AUDIO_SINK_H

#include "../Types.h"

#include <cstddef>

namespace powermac {

class AudioSink {
public:
	virtual						~AudioSink() {}

	// 'frames' holds 'count' interleaved left/right samples.
	virtual	void				Play(const s16* frames, size_t count,
									u32 sampleRate) = 0;
};

}	// namespace powermac

#endif	// PM_AUDIO_SINK_H
