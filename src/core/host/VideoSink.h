/*
 * PowerMac emulator - host side of the emulated display.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_VIDEO_SINK_H
#define PM_VIDEO_SINK_H

#include "../Types.h"

namespace powermac {

class VideoSink {
public:
	virtual ~VideoSink() {}

	// A complete frame, called at the emulated vertical retrace on the
	// emulation thread. 'pixels' holds 0x00RRGGBB host-endian values;
	// 'stride' is in pixels. The buffer is only valid during the call.
	// width or height 0 means the display is off.
	virtual void FrameReady(const u32* pixels, int width, int height,
		int stride) = 0;
};

}	// namespace powermac

#endif	// PM_VIDEO_SINK_H
