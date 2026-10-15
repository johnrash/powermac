/*
 * PowerMac emulator - minimal PNG writer (uncompressed deflate) for
 * screenshots of the emulated display.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_PNG_WRITER_H
#define PM_PNG_WRITER_H

#include "../Types.h"

#include <string>

namespace powermac {

// 'pixels' are 0x00RRGGBB values, 'stride' in pixels.
bool WritePng(const std::string& path, const u32* pixels, int width,
	int height, int stride);

}	// namespace powermac

#endif	// PM_PNG_WRITER_H
