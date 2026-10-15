/*
 * PowerMac emulator - a display card, as the front ends see it.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_DISPLAY_DEVICE_H
#define PM_DISPLAY_DEVICE_H

#include "../Types.h"

namespace powermac {

class VideoSink;

class DisplayDevice {
public:
	virtual ~DisplayDevice() {}

	// Frames go to 'sink' (at each vertical retrace of emulated time).
	virtual void SetVideoSink(VideoSink* sink) = 0;
	// The hardware cursor's hot spot on the screen, false when it is not
	// shown (closed loop mouse moves in the tests and the front ends).
	virtual bool HardwareCursor(int& x, int& y) const = 0;
};

}	// namespace powermac

#endif	// PM_DISPLAY_DEVICE_H
