/*
 * PowerMac emulator - IMS Twin Turbo 128 display card (PCI 10E0:9128).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "ImsTwinTurbo128.h"

#include "../Log.h"
#include "../host/VideoSink.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace powermac {

namespace {

enum {
	kHes = 12, kHeb = 13, kHsb = 14, kHt = 15,
	kVes = 16, kVeb = 17, kVsb = 18, kVt = 19,
	kS1sa = 0, kSp = 2, kDsa = 3, kCnt = 4, kDpOctl = 5, kClr = 6,
	kBltctl = 10,
	kStgctl = 24, kSsr = 25, kSpr = 27, kScr = 35,
	kGioe = 32, kGio = 33, kSstatus = 36, kPrc = 37
};

enum {
	kStgctlTiming = 0x001,
	kStgctlVblEnable = 0x400,
	kSstatusVersion = 0x200,
	kSstatusVbl = 0x010,
	kScrWideMemory = 0x10000,	// both banks interleaved: 8 byte units
	kScrByteSwap = 0x300,		// display byte swap: 1 16-bit, 3 32-bit
	kBltctlFill = 0x200000,
	kBltctlCopy = 0x000005,
	kBltctlRightToLeft = 0x000080
};

}	// namespace


// Sense codes as Apple's monitors wire them (the card's Open Firmware
// code picks its console mode from these).
const ImsTwinTurbo128::Monitor ImsTwinTurbo128::kMonitors[] = {
	{ "Apple 19\" (1024 x 768)", 7, 0x3a },
	{ "Apple Multiple Scan 20 (1152 x 870)", 6, 0x23 },
	{ "Apple 21\" (1152 x 870)", 0, 0x00 },
	{ "Apple 16\" (832 x 624)", 7, 0x2d },
	{ "Apple 13\" (640 x 480, 67 Hz)", 6, 0x00 },
	{ "VGA (640 x 480)", 7, 0x17 },
	{ "No monitor", 7, 0x3f }
};


int
ImsTwinTurbo128::MonitorCount()
{
	return (int)(sizeof(kMonitors) / sizeof(kMonitors[0]));
}


ImsTwinTurbo128::ImsTwinTurbo128(PhysBus& bus, Scheduler& scheduler,
	u64 ticksPerSecond, u32 vramSize, const std::vector<u8>& rom,
	int monitor)
	:
	PciDevice(0x10e0, 0x9128, 0x038000, 0x02),
	fBus(bus),
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fVram(vramSize, 0),
	fRom(rom),
	fMonitor(kMonitors[monitor >= 0 && monitor < MonitorCount() ? monitor
		: 0]),
	fMappedBar(0),
	fMappedRom(0),
	fSink(nullptr)
{
	DeclareBar(0, kBarMemory, kBarSize, true);
	if (!fRom.empty()) {
		u32 romSize = 0x10000;
		while (romSize < fRom.size())
			romSize <<= 1;
		fRom.resize(romSize, 0xff);
		DeclareRomBar(romSize);
	}
	memset(fLoggedMode, 0, sizeof(fLoggedMode));
	fInterruptLine = false;
	SetInterruptPin(1);
	fRetraceTimer.reset(new Timer(scheduler, [this]() { _Retrace(); },
		"tt128-retrace"));
	Reset();
}


ImsTwinTurbo128::~ImsTwinTurbo128()
{
	if (fMappedBar != 0)
		fBus.Unmap(fMappedBar, kBarSize);
	if (fMappedRom != 0)
		fBus.Unmap(fMappedRom, (u32)fRom.size());
}


void
ImsTwinTurbo128::Reset()
{
	memset(fRegisters, 0, sizeof(fRegisters));
	// chip version 2; 8 MB of 8 column VRAM
	StoreLE32(&fRegisters[kSstatus * 4], kSstatusVersion);
	StoreLE32(&fRegisters[kPrc * 4], (fVram.size() >= (8u << 20) ? 0x400 : 0)
		| 0x40);
	_UpdateInterrupt();
	fDac.Reset();
	fDac.SetMonitorAttached(fMonitor.extended != 0x3f);
	fRetraceTimer->Cancel();
	fRetraceTimer->ArmIn(_FrameTicks());
}


void
ImsTwinTurbo128::BarsChanged()
{
	u32 bar = MemoryEnabled() ? BarAddress(0) : 0;
	if (bar != fMappedBar) {
		if (fMappedBar != 0)
			fBus.Unmap(fMappedBar, kBarSize);
		fMappedBar = bar;
		if (bar != 0) {
			fBus.MapMemory(bar, (u32)fVram.size(), fVram.data(), true);
			fBus.MapDevice(bar + kRegisterBase, PhysBus::kPageSize, this);
			fBus.MapDevice(bar + kDacBase, PhysBus::kPageSize, this);
			PM_LOG(kLogVideo, kLogInfo, "twin turbo at %08x", bar);
		}
	}
	u32 rom = RomEnabled() ? RomBarAddress() : 0;
	if (rom != fMappedRom) {
		if (fMappedRom != 0)
			fBus.Unmap(fMappedRom, (u32)fRom.size());
		fMappedRom = rom;
		if (rom != 0) {
			fBus.MapMemory(rom, (u32)fRom.size(), fRom.data(), false);
			PM_LOG(kLogVideo, kLogInfo, "twin turbo ROM at %08x", rom);
		}
	}
}


u8
ImsTwinTurbo128::_SenseLines() const
{
	// the lines the card drives low (bit 0: line 0 = GIO bit 3)
	u8 driven = (u8)((_Register(kGioe) >> 3) & 7);
	u8 extended = fMonitor.extended;
	if ((driven & 4) != 0)
		return (extended >> 4) & 3;
	if ((driven & 2) != 0)
		return (u8)((((extended >> 3) & 1) << 2) | ((extended >> 2) & 1));
	if ((driven & 1) != 0)
		return (u8)((extended & 3) << 1);
	return fMonitor.sense & 7;
}


u32
ImsTwinTurbo128::_ReadRegister(u32 offset, int size)
{
	u8 bytes[4];
	u32 reg = offset & ~3u;
	u32 value = LoadLE32(&fRegisters[reg & 0xff]);
	if (reg == kGio * 4)
		value = (value & ~0x38u) | ((u32)_SenseLines() << 3);
	StoreLE32(bytes, value);
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | bytes[(offset + i) & 3];
	PM_LOG(kLogVideo, kLogTrace, "read %02x = %08x", reg, value);
	return result;
}


void
ImsTwinTurbo128::_WriteRegister(u32 offset, u32 value, int size)
{
	u32 reg = offset & ~3u & 0xff;
	u32 status = _Register(kSstatus);
	for (int i = 0; i < size; i++)
		fRegisters[(offset + i) & 0xff] = (u8)(value >> (8 * (size - 1 - i)));
	if (reg == kSstatus * 4 || reg == kPrc * 4) {
		// read only, but a one written to the vertical blank bit clears it
		if (reg == kSstatus * 4)
			status &= ~(_Register(kSstatus) & kSstatusVbl);
		StoreLE32(&fRegisters[kSstatus * 4], status);
		StoreLE32(&fRegisters[kPrc * 4], (fVram.size() >= (8u << 20)
			? 0x400 : 0) | 0x40);
	}
	if (reg == kSstatus * 4 || reg == kStgctl * 4)
		_UpdateInterrupt();
	// (the drawing engine's registers only when tracing: many writes)
	if (reg <= kBltctl * 4) {
		PM_LOG(kLogVideo, kLogTrace, "write %02x = %08x", reg,
			LoadLE32(&fRegisters[reg]));
	} else {
		PM_LOG(kLogVideo, kLogDebug, "write %02x = %08x", reg,
			LoadLE32(&fRegisters[reg]));
	}
	if (reg == kBltctl * 4 && offset + size >= reg + 4)
		_Blit(LoadLE32(&fRegisters[reg]));
}


// SCR bits 9-8 (Linux's imsttfb "byteswap"): the display reads 32-bit (3)
// or 16-bit (1) pixels with their bytes swapped, so big endian pixels show
// right; Mac OS and BeOS set them for those depths. The display reads the
// card's byte n at n ^ _SwapXor().
u32
ImsTwinTurbo128::_SwapXor() const
{
	switch ((_Register(kScr) & kScrByteSwap) >> 8) {
		case 1: return 1;		// 16-bit
		case 3: return 3;		// 32-bit
	}
	return 0;
}


// The screen refresh and the drawing engine count addresses in 8 bytes
// with both memory banks interleaved (SCR 0x10000; 8 MB cards in the Mac
// OS driver's modes), else in 4.
u32
ImsTwinTurbo128::_AddressUnit() const
{
	return fVram.size() >= (8u << 20)
		&& (_Register(kScr) & kScrWideMemory) != 0 ? 8 : 4;
}


// The drawing engine, in one step (the busy bits never show): a solid
// fill of a rectangle, or a copy of one (screen to screen, either
// direction). Byte offsets in video memory; CNT is (rows - 1) << 16 |
// (bytes - 1, negative from right to left); the pitches are signed steps
// in the low 16 bits (SP's high half is the source pitch). After the
// register descriptions in Linux's imsttfb.c.
void
ImsTwinTurbo128::_Blit(u32 control)
{
	u32 mask = (u32)fVram.size() - 1;
	u32 count = _Register(kCnt);
	int rows = (int)(count >> 16) + 1;
	int bytes = std::abs((int)(s16)(count & 0xffff)) + 1;
	u32 destination = _Register(kDsa);
	int destinationStep = (s16)(_Register(kDpOctl) & 0xffff);
	if ((control & kBltctlFill) != 0 || (control & 0x840) == 0x840) {
		// the color repeats every 4 bytes, little endian (BeOS swaps its
		// 16 and 32-bit colors before it loads CLR)
		u32 color = _Register(kClr);
		for (int y = 0; y < rows; y++) {
			u32 line = destination + (u32)(y * destinationStep);
			for (int x = 0; x < bytes; x++) {
				u32 address = (line + x) & mask;
				fVram[address] = (u8)(color >> (8 * (address & 3)));
			}
		}
		PM_LOG(kLogVideo, kLogDebug, "fill %d x %d at %06x", bytes, rows,
			destination);
		return;
	}
	if ((control & 0x0f) == kBltctlCopy) {
		u32 source = _Register(kS1sa);
		int sourceStep = (s16)(_Register(kSp) & 0xffff);
		int direction = (control & kBltctlRightToLeft) != 0 ? -1 : 1;
		for (int y = 0; y < rows; y++) {
			u32 from = source + (u32)(y * sourceStep);
			u32 to = destination + (u32)(y * destinationStep);
			if (direction > 0) {
				for (int x = 0; x < bytes; x++)
					fVram[(to + x) & mask] = fVram[(from + x) & mask];
			} else {
				for (int x = 0; x < bytes; x++)
					fVram[(to - x) & mask] = fVram[(from - x) & mask];
			}
		}
		PM_LOG(kLogVideo, kLogDebug, "copy %d x %d from %06x to %06x%s",
			bytes, rows, source, destination, direction < 0 ? " (back)"
				: "");
		return;
	}
	PM_LOG(kLogVideo, kLogInfo, "drawing engine: operation %08x not "
		"emulated", control);
}


u32
ImsTwinTurbo128::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = addr - fMappedBar;
	if (offset >= kDacBase && offset < kDacBase + 0x40) {
		u8 value = fDac.Read((offset - kDacBase) >> 2);
		// the value is the low byte of a little endian word
		u8 bytes[4] = { value, 0, 0, 0 };
		u32 result = 0;
		for (int i = 0; i < size; i++)
			result = (result << 8) | bytes[(offset + i) & 3];
		return result;
	}
	if (offset >= kRegisterBase && offset < kRegisterBase + 0x100)
		return _ReadRegister(offset - kRegisterBase, size);
	PM_LOG(kLogVideo, kLogDebug, "read%d of %06x", size * 8, offset);
	return 0;
}


void
ImsTwinTurbo128::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 offset = addr - fMappedBar;
	if (offset >= kDacBase && offset < kDacBase + 0x40) {
		// the byte at the word's first address
		int lane = offset & 3;
		u8 byte = (u8)(value >> (8 * (size - 1)));
		if (lane == 0)
			fDac.Write((offset - kDacBase) >> 2, byte);
		return;
	}
	if (offset >= kRegisterBase && offset < kRegisterBase + 0x100) {
		_WriteRegister(offset - kRegisterBase, value, size);
		return;
	}
	PM_LOG(kLogVideo, kLogDebug, "write%d of %06x = %x", size * 8, offset,
		value);
}


bool
ImsTwinTurbo128::HardwareCursor(int& x, int& y) const
{
	if (fDac.CursorMode() == 0)
		return false;
	x = fDac.CursorX();
	y = fDac.CursorY();
	return true;
}


u64
ImsTwinTurbo128::_FrameTicks() const
{
	double clock = fDac.PixelClockHz();
	u32 ht = _Register(kHt);
	u32 vt = _Register(kVt);
	double pixels = (double)ht * 16 * vt;
	double rate = clock > 0 && pixels > 0 ? clock / pixels : 0;
	if (rate < 30 || rate > 200)
		rate = 60;
	return (u64)(fTicksPerSecond / rate);
}


void
ImsTwinTurbo128::_Retrace()
{
	fRetraceTimer->ArmIn(_FrameTicks());
	if ((_Register(kStgctl) & kStgctlTiming) != 0) {
		StoreLE32(&fRegisters[kSstatus * 4],
			_Register(kSstatus) | kSstatusVbl);
		_UpdateInterrupt();
	}
	_Render();
}


void
ImsTwinTurbo128::_UpdateInterrupt()
{
	bool level = (_Register(kSstatus) & kSstatusVbl) != 0
		&& (_Register(kStgctl) & kStgctlVblEnable) != 0;
	if (level != fInterruptLine) {
		fInterruptLine = level;
		PM_LOG(kLogVideo, kLogTrace, "vbl interrupt %s at %.3f ms",
			level ? "raised" : "cleared",
			fScheduler.Now() * 1000.0 / fTicksPerSecond);
		if (fInterrupt)
			fInterrupt(level);
	}
}


void
ImsTwinTurbo128::_Render()
{
	if (fSink == nullptr)
		return;
	int bpp = fDac.BitsPerPixel();
	int width = (int)(_Register(kHsb) - _Register(kHeb)) * 16;
	int height = (int)(_Register(kVsb) - _Register(kVeb));
	if ((_Register(kStgctl) & 1) == 0 || width <= 0 || height <= 0
		|| width > 2048 || height > 2048) {
		fSink->FrameReady(nullptr, 0, 0, 0);
		return;
	}
	u32 unit = _AddressUnit();
	u32 pitch = _Register(kSpr) * unit;
	if (pitch == 0)
		pitch = (u32)width * bpp / 8;
	u32 start = _Register(kSsr) * unit;
	u32 current[6] = { (u32)width, (u32)height, (u32)bpp, pitch, start,
		fDac.DirectColor() };
	if (memcmp(current, fLoggedMode, sizeof(current)) != 0) {
		memcpy(fLoggedMode, current, sizeof(current));
		PM_LOG(kLogVideo, kLogInfo, "%dx%d, %d bits%s, pitch %u, start %06x",
			width, height, bpp, current[5] ? " direct" : "", pitch, start);
	}

	fFrame.resize((size_t)width * height);
	const u32* palette = fDac.Palette();
	u8 mask = fDac.PixelMask();
	bool direct = fDac.DirectColor();
	Tvp3030::PixelFormat format = fDac.Format();
	u32 vramMask = (u32)fVram.size() - 1;
	u32 swap = _SwapXor();
	for (int y = 0; y < height; y++) {
		u32* out = &fFrame[(size_t)y * width];
		u32 line = start + (u32)y * pitch;
		for (int x = 0; x < width; x++) {
			u32 color;
			switch (format) {
				case Tvp3030::kIndexed8:
					color = palette[fVram[(line + x) & vramMask] & mask];
					break;
				case Tvp3030::kRgb1555:
				case Tvp3030::kRgb565:
				{
					u32 a = line + x * 2;
					u32 pixel = (fVram[((a + 1) ^ swap) & vramMask] << 8)
						| fVram[(a ^ swap) & vramMask];
					u32 r, g, b;
					if (format == Tvp3030::kRgb1555) {
						r = (pixel >> 10) & 31;
						g = (pixel >> 5) & 31;
						b = pixel & 31;
						g = (g << 3) | (g >> 2);
					} else {
						r = (pixel >> 11) & 31;
						g = (pixel >> 5) & 63;
						b = pixel & 31;
						g = (g << 2) | (g >> 4);
					}
					r = (r << 3) | (r >> 2);
					b = (b << 3) | (b >> 2);
					color = (r << 16) | (g << 8) | b;
					break;
				}
				default:
				{
					u32 a = line + x * 4;
					color = ((u32)fVram[((a + 2) ^ swap) & vramMask] << 16)
						| (fVram[((a + 1) ^ swap) & vramMask] << 8)
						| fVram[(a ^ swap) & vramMask];
					break;
				}
			}
			if (direct && format != Tvp3030::kIndexed8) {
				color = (palette[(color >> 16) & 0xff] & 0xff0000)
					| (palette[(color >> 8) & 0xff] & 0x00ff00)
					| (palette[color & 0xff] & 0x0000ff);
			}
			out[x] = color;
		}
	}

	int mode = fDac.CursorMode();
	if (mode != 0) {
		int cx = fDac.CursorX();
		int cy = fDac.CursorY();
		for (int y = 0; y < 64; y++) {
			int sy = cy + y;
			if (sy < 0 || sy >= height)
				continue;
			for (int x = 0; x < 64; x++) {
				int sx = cx + x;
				if (sx < 0 || sx >= width)
					continue;
				int pixel = fDac.CursorPixel(x, y);
				u32& target = fFrame[(size_t)sy * width + sx];
				switch (mode) {
					case 1:		// three colors
						if (pixel != 0)
							target = fDac.CursorColor(pixel);
						break;
					case 2:		// XGA
						if (pixel < 2)
							target = fDac.CursorColor(pixel + 1);
						else if (pixel == 3)
							target ^= 0xffffff;
						break;
					default:	// X-Windows
						if (pixel >= 2)
							target = fDac.CursorColor(pixel - 1);
						break;
				}
			}
		}
	}
	fSink->FrameReady(fFrame.data(), width, height, width);
}

}	// namespace powermac
