/*
 * PowerMac emulator - TI TVP3030 RAMDAC (the Twin Turbo 128's "zelea").
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Direct registers (one per 32-bit word on the card, the value in the
 * low byte):
 *   0 palette write address / index of the indirect registers
 *   1 palette data (R, G, B in turn; 8-bit DAC)
 *   2 pixel read mask        3 palette read address
 *   4 cursor color write address   5 cursor color data
 *   7 cursor color read address    9 direct cursor control
 *  10 indirect register data      11 cursor RAM data
 *  12-15 cursor X low, X high, Y low, Y high
 * Indirect registers (index in register 0): 0x06 cursor control, 0x07
 * byte router, 0x0F latch control, 0x18 true color control, 0x19
 * multiplex control, 0x1A clock select, 0x1C palette page, 0x1D general
 * control, 0x1E misc control, 0x2C PLL address, 0x2D/0x2E/0x2F the pixel,
 * memory and loop clock PLL data (N, M, P, then the status: bit 6 is
 * lock), 0x39 MCLK/LCLK control, 0x3A sense test (bit 0 clear: a monitor
 * loads the outputs), 0x3F the ID (0x30).
 *
 * PLL output: 8 * 14.31818 MHz * (65 - M) / (65 - N) / 2^P.
 */
#ifndef PM_TVP3030_H
#define PM_TVP3030_H

#include "../Types.h"

namespace powermac {

class Tvp3030 {
public:
	enum PixelFormat {
		kIndexed8,		// through the palette
		kRgb1555,		// big endian x-R-G-B 1-5-5-5
		kRgb565,
		kRgbx8888		// big endian x-R-G-B 8-8-8-8
	};

								Tvp3030();

			void				Reset();
			u8					Read(int reg);
			void				Write(int reg, u8 value);

			// For the display.
			PixelFormat			Format() const;
			int					BitsPerPixel() const;
			// Direct color: the components go through the palette (gamma).
			bool				DirectColor() const
									{ return (fIndirect[0x18] & 0x40) != 0; }
			const u32*			Palette() const { return fPalette; }
			u8					PixelMask() const { return fPixelMask; }
			double				PixelClockHz() const;

			// The cursor: off, or its mode (1: three colors, 2: XGA, 3:
			// X-Windows), position (of its 64x64 square's top left) and
			// pixels (2 bits each, plane 1 << 1 | plane 0).
			int					CursorMode() const;
			int					CursorX() const
									{ return (int)((fCursorX[1] << 8)
										| fCursorX[0]) - 64; }
			int					CursorY() const
									{ return (int)((fCursorY[1] << 8)
										| fCursorY[0]) - 64; }
			int					CursorPixel(int x, int y) const;
			u32					CursorColor(int index) const
									{ return fCursorColors[index & 3]; }

			void				SetMonitorAttached(bool attached)
									{ fMonitorAttached = attached; }

private:
			u8					_ReadIndirect(u8 index);
			void				_WriteIndirect(u8 index, u8 value);
			int					_CursorAddressHigh() const;

			u8					fIndex;			// = palette write address
			u8					fReadAddress;
			int					fWriteComponent;
			int					fReadComponent;
			u8					fComponents[3];
			u32					fPalette[256];	// 0x00RRGGBB
			u8					fPixelMask;

			u8					fCursorColorAddress;
			int					fCursorColorComponent;
			u8					fCursorColorComponents[3];
			u32					fCursorColors[4];	// overscan, 1, 2, 3
			u8					fCursorRam[1024];
			u16					fCursorRamAddress;
			u8					fCursorX[2];
			u8					fCursorY[2];

			u8					fIndirect[256];
			u8					fPll[3][3];		// pixel, memory, loop: N M P
			u8					fPllPointer[3];
			bool				fMonitorAttached;
};

}	// namespace powermac

#endif	// PM_TVP3030_H
