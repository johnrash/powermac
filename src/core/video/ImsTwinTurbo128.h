/*
 * PowerMac emulator - IMS Twin Turbo 128 display card (PCI 10E0:9128).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * The card the Power Macintosh 9500 shipped with (the "M8A": 8 MB of
 * VRAM and a TVP3030 RAMDAC). Its expansion ROM (64 KB, dumped from a
 * real card) holds the Open Firmware FCode and the Mac OS driver; the
 * emulation is of the chip, as those program it.
 *
 * BAR 0 (32 MB, prefetchable):
 *   0x000000  video memory
 *   0x800000  the chip's registers ("cosmo"), 32 bits little endian, one
 *             every 4 bytes: 0x00-0x2C the drawing engine, 0x30-0x4C the
 *             timing (HES, HEB, HSB, HT, VES, VEB, VSB, VT), 0x50 HCIV,
 *             0x54 VCIV, 0x58 TCDR, 0x5C VIL, 0x60 STGCTL (bit 0: timing
 *             on), 0x64 SSR (start), 0x68 HRIR, 0x6C SPR (pitch), 0x70 CMR,
 *             0x74 SRGCTL, 0x78 RRCIV, 0x7C RRSC, 0x80 GIOE and 0x84 GIO
 *             (the monitor sense lines, bits 3-5), 0x88 RRCR, 0x8C SCR
 *             (0x10000: both memory banks interleaved, addresses in 8
 *             byte units, else 4; bits 9-8: the display swaps the bytes
 *             of 32-bit (3) or 16-bit (1) pixels),
 *             0x90 SSTATUS (bits 8-11: chip version 2; 0x10: vertical
 *             blank, a written one clears it), 0x94 PRC (bit 0x400: 8 MB,
 *             0x40: 8 column VRAM)
 *   0x840000  the RAMDAC (Tvp3030.h), a register every 4 bytes
 *
 * The vertical blank interrupt (INTA) is SSTATUS 0x10 while STGCTL 0x400
 * enables it; the Mac OS driver loads the CLUT from its handler, so
 * without it the palette keeps the gray it fills before drawing (as in
 * the driver's handler at code+0x7F30, enabler at code+0x802C).
 *
 * Pixels are big endian in video memory for both systems: they set SCR's
 * byte swap for their depth (Mac OS 0x153DC in millions of colors, BeOS
 * 0xD398), which makes the little endian display read them right. The
 * drawing engine stores CLR little endian; BeOS swaps its 16 and 32-bit
 * colors before loading it (its IXMicro add-on calls __swap_int32). Mac OS
 * does not use the engine.
 *
 * The display: VSB - VEB lines of (HSB - HEB) units of 16 pixels (at any
 * depth: the Mac OS driver's 1024 x 768 x 32 has 64 units, Open
 * Firmware's 640 x 480 x 8 has 40), SPR address units a line, from SSR;
 * the totals (HT, VT) and the pixel clock give the rate.
 *
 * The drawing engine (0x00-0x28: S1SA source, SP source pitch, DSA
 * destination, CNT size, DP_OCTL destination pitch, CLR color, BLTCTL
 * starts it) fills and copies rectangles (BeOS's accelerated driver).
 *
 * The monitor answers the sense lines as an Apple monitor would: its
 * sense code with no line driven, and bits of its extended code while the
 * card drives one line low.
 */
#ifndef PM_IMS_TWIN_TURBO_128_H
#define PM_IMS_TWIN_TURBO_128_H

#include "DisplayDevice.h"
#include "Tvp3030.h"
#include "../board/Scheduler.h"
#include "../bus/PciBus.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <functional>
#include <memory>
#include <vector>

namespace powermac {

class ImsTwinTurbo128 : public PciDevice, public MmioDevice,
	public DisplayDevice {
public:
	static const u32 kBarSize = 0x2000000;
	static const u32 kRegisterBase = 0x800000;
	static const u32 kDacBase = 0x840000;

	// Monitors (sense code, extended sense code).
	struct Monitor {
		const char*	name;
		u8			sense;
		u8			extended;
	};
	static const Monitor kMonitors[];
	static int MonitorCount();

								ImsTwinTurbo128(PhysBus& bus,
									Scheduler& scheduler,
									u64 ticksPerSecond, u32 vramSize,
									const std::vector<u8>& rom,
									int monitor);
	virtual						~ImsTwinTurbo128();

			void				Reset();
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }

	// DisplayDevice
	virtual	void				SetVideoSink(VideoSink* sink)
									{ fSink = sink; }
	virtual	bool				HardwareCursor(int& x, int& y) const;

			// The video memory (tests).
			const std::vector<u8>& Vram() const { return fVram; }

	// MmioDevice
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "twin-turbo"; }

	// PciDevice
	virtual	const char*			PciName() const { return "twin-turbo-128"; }

protected:
	virtual	void				BarsChanged();

private:
			u32					_Register(int index) const
									{ return LoadLE32(&fRegisters[index * 4]); }
			u32					_ReadRegister(u32 offset, int size);
			void				_WriteRegister(u32 offset, u32 value,
									int size);
			u8					_SenseLines() const;
			u64					_FrameTicks() const;
			void				_Retrace();
			void				_UpdateInterrupt();
			u32					_AddressUnit() const;
			u32					_SwapXor() const;
			void				_Blit(u32 control);
			void				_Render();

			PhysBus&			fBus;
			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			std::vector<u8>		fVram;
			std::vector<u8>		fRom;
			Monitor				fMonitor;
			Tvp3030				fDac;
			u8					fRegisters[0x100];
			u32					fMappedBar;		// 0: not mapped
			u32					fMappedRom;
			VideoSink*			fSink;
			std::function<void(bool)> fInterrupt;
			bool				fInterruptLine;
			std::unique_ptr<Timer> fRetraceTimer;
			std::vector<u32>	fFrame;
			u32					fLoggedMode[6];	// the mode last logged
};

}	// namespace powermac

#endif	// PM_IMS_TWIN_TURBO_128_H
