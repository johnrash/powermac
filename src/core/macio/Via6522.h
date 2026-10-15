/*
 * PowerMac emulator - the VIA (6522 versatile interface adapter).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * In Grand Central at 0x16000; register n at n * 0x200 (the Mac's VIA
 * register spacing). Clocked at 783.36 kHz (C16M / 20): the ROM measures
 * the bus clock against timer 1, the Time Manager's code timer 2.
 *
 * Port B and the shift register talk to the Cuda microcontroller: PB3 is
 * TREQ (input), PB4 BYTEACK and PB5 TIP (outputs), the shift register
 * moves the bytes under Cuda's clock (CB1). The attached peripheral sees
 * port B writes and shift register accesses and answers with ShiftIn()
 * and ShiftDone().
 */
#ifndef PM_VIA6522_H
#define PM_VIA6522_H

#include "../board/Scheduler.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <functional>
#include <memory>

namespace powermac {

class Via6522 : public MmioDevice {
public:
	static const u32 kClockHz = 783360;

	enum {
		kIfrCa2 = 0x01,
		kIfrCa1 = 0x02,
		kIfrShift = 0x04,
		kIfrCb2 = 0x08,
		kIfrCb1 = 0x10,
		kIfrTimer2 = 0x20,
		kIfrTimer1 = 0x40
	};

	// The device on port B and the shift register (Cuda).
	class Peripheral {
	public:
		virtual ~Peripheral() {}
		// Port B as driven: outputs from ORB, inputs as the peripheral
		// drives them.
		virtual void PortBWritten(u8 value) = 0;
	};

								Via6522(Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Via6522();

			void				Reset();
			void				SetPeripheral(Peripheral* peripheral)
									{ fPeripheral = peripheral; }
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }

			// Port B pins the peripheral drives (where DDRB makes them
			// inputs).
			void				SetPortBInput(u8 mask, u8 value);
			u8					PortB() const;
			// The shift register's direction (ACR bit 4: out to the
			// peripheral).
			bool				ShiftOut() const
									{ return (fAcr & 0x10) != 0; }
			u8					ShiftRegister() const { return fShift; }
			// The peripheral shifted a byte in (input mode) or finished
			// shifting it out: sets the shift register flag.
			void				ShiftIn(u8 value);
			void				ShiftDone();

			u8					Ifr() const;

	// MmioDevice (offsets within the VIA's 8 KB)
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "via"; }

			u8					Read(int reg);
			void				Write(int reg, u8 value);

private:
			u64					_Cycles(u64 ticks) const;
			u64					_Ticks(u64 cycles) const;
			u16					_Timer1Counter() const;
			u16					_Timer2Counter() const;
			void				_StartTimer1();
			void				_Timer1Expired();
			void				_Timer2Expired();
			void				_SetFlags(u8 flags);
			void				_ClearFlags(u8 flags);
			void				_UpdateInterrupt();

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			Peripheral*			fPeripheral;
			std::function<void(bool)> fInterrupt;
			bool				fInterruptLine;

			u8					fOrb;
			u8					fOra;
			u8					fDdrb;
			u8					fDdra;
			u8					fPortBInput;
			u8					fShift;
			u8					fAcr;
			u8					fPcr;
			u8					fIfr;
			u8					fIer;

			u16					fTimer1Latch;
			u64					fTimer1Start;	// ticks when loaded
			u16					fTimer1Value;	// loaded value
			bool				fTimer1Armed;	// one shot not fired yet
			u16					fTimer2LatchLow;
			u64					fTimer2Start;
			u16					fTimer2Value;
			bool				fTimer2Armed;
			std::unique_ptr<Timer> fTimer1;
			std::unique_ptr<Timer> fTimer2;
};

}	// namespace powermac

#endif	// PM_VIA6522_H
