/*
 * PowerMac emulator - TI TVP3030 RAMDAC (the Twin Turbo 128's "zelea").
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Tvp3030.h"

#include "../Log.h"

namespace powermac {

namespace {

enum {
	kRegPaletteWrite = 0,
	kRegPaletteData = 1,
	kRegPixelMask = 2,
	kRegPaletteRead = 3,
	kRegCursorColorWrite = 4,
	kRegCursorColorData = 5,
	kRegCursorColorRead = 7,
	kRegCursorControl = 9,
	kRegIndexedData = 10,
	kRegCursorRam = 11,
	kRegCursorXLow = 12,
	kRegCursorXHigh = 13,
	kRegCursorYLow = 14,
	kRegCursorYHigh = 15
};

enum {
	kIndCursorControl = 0x06,
	kIndTrueColor = 0x18,
	kIndPllAddress = 0x2c,
	kIndPixelPll = 0x2d,
	kIndMemoryPll = 0x2e,
	kIndLoopPll = 0x2f,
	kIndSenseTest = 0x3a,
	kIndId = 0x3f,
	kIndReset = 0xff
};

const double kReferenceHz = 14318180.0;

}	// namespace


Tvp3030::Tvp3030()
{
	Reset();
}


void
Tvp3030::Reset()
{
	fIndex = 0;
	fReadAddress = 0;
	fWriteComponent = fReadComponent = 0;
	memset(fComponents, 0, sizeof(fComponents));
	for (int i = 0; i < 256; i++)
		fPalette[i] = 0;
	fPixelMask = 0xff;
	fCursorColorAddress = 0;
	fCursorColorComponent = 0;
	memset(fCursorColorComponents, 0, sizeof(fCursorColorComponents));
	memset(fCursorColors, 0, sizeof(fCursorColors));
	memset(fCursorRam, 0, sizeof(fCursorRam));
	fCursorRamAddress = 0;
	memset(fCursorX, 0, sizeof(fCursorX));
	memset(fCursorY, 0, sizeof(fCursorY));
	memset(fIndirect, 0, sizeof(fIndirect));
	fIndirect[kIndTrueColor] = 0x80;
	memset(fPll, 0, sizeof(fPll));
	memset(fPllPointer, 0, sizeof(fPllPointer));
	fMonitorAttached = true;
}


Tvp3030::PixelFormat
Tvp3030::Format() const
{
	u8 control = fIndirect[kIndTrueColor];
	if ((control & 0x80) != 0)
		return kIndexed8;
	switch (control & 0x0f) {
		case 0x04: return kRgb1555;
		case 0x05: return kRgb565;
		case 0x06: case 0x07: return kRgbx8888;
	}
	return kIndexed8;
}


int
Tvp3030::BitsPerPixel() const
{
	switch (Format()) {
		case kRgb1555: case kRgb565: return 16;
		case kRgbx8888: return 32;
		default: return 8;
	}
}


double
Tvp3030::PixelClockHz() const
{
	u32 n = fPll[0][0] & 0x3f;
	u32 m = fPll[0][1] & 0x3f;
	u32 p = fPll[0][2] & 3;
	if (n >= 65)
		return 0;
	return 8.0 * kReferenceHz * (65 - m) / (65 - n) / (1 << p);
}


int
Tvp3030::CursorMode() const
{
	return fIndirect[kIndCursorControl] & 3;
}


int
Tvp3030::_CursorAddressHigh() const
{
	return (fIndirect[kIndCursorControl] >> 2) & 3;
}


int
Tvp3030::CursorPixel(int x, int y) const
{
	// plane 0 in the first 512 bytes, plane 1 in the next; 8 bytes a row,
	// the leftmost pixel in bit 7
	int offset = y * 8 + x / 8;
	int bit = 7 - (x & 7);
	int plane0 = (fCursorRam[offset] >> bit) & 1;
	int plane1 = (fCursorRam[512 + offset] >> bit) & 1;
	return (plane1 << 1) | plane0;
}


u8
Tvp3030::Read(int reg)
{
	switch (reg) {
		case kRegPaletteWrite:
			return fIndex;
		case kRegPaletteData:
		{
			u32 color = fPalette[fReadAddress];
			u8 value = (u8)(color >> (16 - 8 * fReadComponent));
			if (++fReadComponent == 3) {
				fReadComponent = 0;
				fReadAddress++;
			}
			return value;
		}
		case kRegPixelMask:
			return fPixelMask;
		case kRegPaletteRead:
			return fReadAddress;
		case kRegCursorColorWrite:
		case kRegCursorColorRead:
			return fCursorColorAddress;
		case kRegCursorColorData:
		{
			u32 color = fCursorColors[fCursorColorAddress & 3];
			u8 value = (u8)(color >> (16 - 8 * fCursorColorComponent));
			if (++fCursorColorComponent == 3) {
				fCursorColorComponent = 0;
				fCursorColorAddress++;
			}
			return value;
		}
		case kRegCursorControl:
			return fIndirect[kIndCursorControl];
		case kRegIndexedData:
			return _ReadIndirect(fIndex);
		case kRegCursorRam:
		{
			u16 address = (u16)((_CursorAddressHigh() << 8) | fIndex);
			u8 value = fCursorRam[address & 0x3ff];
			address = (address + 1) & 0x3ff;
			fIndex = (u8)address;
			fIndirect[kIndCursorControl] = (fIndirect[kIndCursorControl]
				& ~0x0c) | ((address >> 6) & 0x0c);
			return value;
		}
		case kRegCursorXLow: return fCursorX[0];
		case kRegCursorXHigh: return fCursorX[1];
		case kRegCursorYLow: return fCursorY[0];
		case kRegCursorYHigh: return fCursorY[1];
	}
	return 0;
}


void
Tvp3030::Write(int reg, u8 value)
{
	switch (reg) {
		case kRegPaletteWrite:
			fIndex = value;
			fWriteComponent = 0;
			break;
		case kRegPaletteData:
			fComponents[fWriteComponent++] = value;
			if (fWriteComponent == 3) {
				fPalette[fIndex] = ((u32)fComponents[0] << 16)
					| (fComponents[1] << 8) | fComponents[2];
				PM_LOG(kLogVideo, kLogTrace, "tvp3030 palette %02x = %06x",
					fIndex, fPalette[fIndex]);
				fWriteComponent = 0;
				fIndex++;
			}
			break;
		case kRegPixelMask:
			fPixelMask = value;
			break;
		case kRegPaletteRead:
			fReadAddress = value;
			fReadComponent = 0;
			break;
		case kRegCursorColorWrite:
		case kRegCursorColorRead:
			fCursorColorAddress = value;
			fCursorColorComponent = 0;
			break;
		case kRegCursorColorData:
			fCursorColorComponents[fCursorColorComponent++] = value;
			if (fCursorColorComponent == 3) {
				fCursorColors[fCursorColorAddress & 3]
					= ((u32)fCursorColorComponents[0] << 16)
					| (fCursorColorComponents[1] << 8)
					| fCursorColorComponents[2];
				fCursorColorComponent = 0;
				fCursorColorAddress++;
			}
			break;
		case kRegCursorControl:
			// the direct register mirrors the indirect one
			fIndirect[kIndCursorControl] = value;
			break;
		case kRegIndexedData:
			_WriteIndirect(fIndex, value);
			break;
		case kRegCursorRam:
		{
			u16 address = (u16)((_CursorAddressHigh() << 8) | fIndex);
			fCursorRam[address & 0x3ff] = value;
			address = (address + 1) & 0x3ff;
			fIndex = (u8)address;
			fIndirect[kIndCursorControl] = (fIndirect[kIndCursorControl]
				& ~0x0c) | ((address >> 6) & 0x0c);
			break;
		}
		case kRegCursorXLow: fCursorX[0] = value; break;
		case kRegCursorXHigh: fCursorX[1] = value & 0x0f; break;
		case kRegCursorYLow: fCursorY[0] = value; break;
		case kRegCursorYHigh: fCursorY[1] = value & 0x0f; break;
	}
}


u8
Tvp3030::_ReadIndirect(u8 index)
{
	switch (index) {
		case kIndPixelPll:
		case kIndMemoryPll:
		case kIndLoopPll:
		{
			int pll = index - kIndPixelPll;
			u8& pointer = fPllPointer[pll];
			u8 value = pointer < 3 ? fPll[pll][pointer] : 0x40;	// locked
			pointer = (pointer + 1) & 3;
			return value;
		}
		case kIndPllAddress:
			return (u8)(fPllPointer[0] | (fPllPointer[1] << 2)
				| (fPllPointer[2] << 4));
		case kIndSenseTest:
			return fMonitorAttached ? 0x00 : 0x01;
		case kIndId:
			return 0x30;
	}
	return fIndirect[index];
}


void
Tvp3030::_WriteIndirect(u8 index, u8 value)
{
	PM_LOG(kLogVideo, kLogDebug, "tvp3030 indirect %02x = %02x", index,
		value);
	switch (index) {
		case kIndPixelPll:
		case kIndMemoryPll:
		case kIndLoopPll:
		{
			int pll = index - kIndPixelPll;
			u8& pointer = fPllPointer[pll];
			if (pointer < 3)
				fPll[pll][pointer] = value;
			pointer = (pointer + 1) & 3;
			return;
		}
		case kIndPllAddress:
			fPllPointer[0] = value & 3;
			fPllPointer[1] = (value >> 2) & 3;
			fPllPointer[2] = (value >> 4) & 3;
			return;
		case kIndId:
			return;
		case kIndReset:
			// a write resets the chip's state machines (nothing to do)
			return;
	}
	fIndirect[index] = value;
}

}	// namespace powermac
