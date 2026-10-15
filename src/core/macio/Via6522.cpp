/*
 * PowerMac emulator - the VIA (6522 versatile interface adapter).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Via6522.h"

#include "../Log.h"

namespace powermac {

enum {
	kRegOrb = 0,
	kRegOra = 1,
	kRegDdrb = 2,
	kRegDdra = 3,
	kRegT1CounterLow = 4,
	kRegT1CounterHigh = 5,
	kRegT1LatchLow = 6,
	kRegT1LatchHigh = 7,
	kRegT2CounterLow = 8,
	kRegT2CounterHigh = 9,
	kRegShift = 10,
	kRegAcr = 11,
	kRegPcr = 12,
	kRegIfr = 13,
	kRegIer = 14,
	kRegOraNoHandshake = 15
};

// A timer interrupts this many cycles after it was loaded with N, and
// N + 2 cycles make a free running period (the 6522's reload cycles).
static const u64 kTimerExtraCycles = 2;


Via6522::Via6522(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fPeripheral(nullptr),
	fInterruptLine(false)
{
	fTimer1.reset(new Timer(scheduler, [this]() { _Timer1Expired(); },
		"via-t1"));
	fTimer2.reset(new Timer(scheduler, [this]() { _Timer2Expired(); },
		"via-t2"));
	Reset();
}


Via6522::~Via6522()
{
}


void
Via6522::Reset()
{
	fOrb = fOra = 0;
	fDdrb = fDdra = 0;
	fPortBInput = 0xff;
	fShift = 0;
	fAcr = fPcr = 0;
	fIfr = fIer = 0;
	fTimer1Latch = 0xffff;
	fTimer1Start = fScheduler.Now();
	fTimer1Value = 0xffff;
	fTimer1Armed = false;
	fTimer2LatchLow = 0xff;
	fTimer2Start = fScheduler.Now();
	fTimer2Value = 0xffff;
	fTimer2Armed = false;
	fTimer1->Cancel();
	fTimer2->Cancel();
	_UpdateInterrupt();
}


u64
Via6522::_Cycles(u64 ticks) const
{
	return ticks * kClockHz / fTicksPerSecond;
}


u64
Via6522::_Ticks(u64 cycles) const
{
	// rounded up: the timer has expired when the event fires
	return (cycles * fTicksPerSecond + kClockHz - 1) / kClockHz;
}


u16
Via6522::_Timer1Counter() const
{
	u64 elapsed = _Cycles(fScheduler.Now() - fTimer1Start);
	return (u16)(fTimer1Value - elapsed);
}


u16
Via6522::_Timer2Counter() const
{
	u64 elapsed = _Cycles(fScheduler.Now() - fTimer2Start);
	return (u16)(fTimer2Value - elapsed);
}


void
Via6522::_StartTimer1()
{
	fTimer1Start = fScheduler.Now();
	fTimer1Value = fTimer1Latch;
	fTimer1Armed = true;
	fTimer1->Arm(fTimer1Start + _Ticks(fTimer1Value + kTimerExtraCycles));
}


void
Via6522::_Timer1Expired()
{
	_SetFlags(kIfrTimer1);
	if ((fAcr & 0x40) != 0) {
		// free running: reload from the latch
		fTimer1Start = fTimer1->When();
		fTimer1Value = fTimer1Latch;
		fTimer1->Arm(fTimer1Start + _Ticks(fTimer1Value
			+ kTimerExtraCycles));
	} else
		fTimer1Armed = false;
}


void
Via6522::_Timer2Expired()
{
	_SetFlags(kIfrTimer2);
	fTimer2Armed = false;
}


void
Via6522::_SetFlags(u8 flags)
{
	fIfr |= flags;
	_UpdateInterrupt();
}


void
Via6522::_ClearFlags(u8 flags)
{
	fIfr &= ~flags;
	_UpdateInterrupt();
}


u8
Via6522::Ifr() const
{
	u8 value = fIfr & 0x7f;
	if ((value & fIer) != 0)
		value |= 0x80;
	return value;
}


void
Via6522::_UpdateInterrupt()
{
	bool line = (fIfr & fIer & 0x7f) != 0;
	if (line == fInterruptLine)
		return;
	fInterruptLine = line;
	if (fInterrupt)
		fInterrupt(line);
}


void
Via6522::SetPortBInput(u8 mask, u8 value)
{
	fPortBInput = (fPortBInput & ~mask) | (value & mask);
}


u8
Via6522::PortB() const
{
	return (fOrb & fDdrb) | (fPortBInput & ~fDdrb);
}


void
Via6522::ShiftIn(u8 value)
{
	fShift = value;
	_SetFlags(kIfrShift);
}


void
Via6522::ShiftDone()
{
	_SetFlags(kIfrShift);
}


u8
Via6522::Read(int reg)
{
	switch (reg) {
		case kRegOrb:
			_ClearFlags(kIfrCb1 | kIfrCb2);
			return PortB();
		case kRegOra:
			_ClearFlags(kIfrCa1 | kIfrCa2);
			return (fOra & fDdra) | ~fDdra;
		case kRegOraNoHandshake:
			return (fOra & fDdra) | ~fDdra;
		case kRegDdrb:
			return fDdrb;
		case kRegDdra:
			return fDdra;
		case kRegT1CounterLow:
			_ClearFlags(kIfrTimer1);
			return (u8)_Timer1Counter();
		case kRegT1CounterHigh:
			return (u8)(_Timer1Counter() >> 8);
		case kRegT1LatchLow:
			return (u8)fTimer1Latch;
		case kRegT1LatchHigh:
			return (u8)(fTimer1Latch >> 8);
		case kRegT2CounterLow:
			_ClearFlags(kIfrTimer2);
			return (u8)_Timer2Counter();
		case kRegT2CounterHigh:
			return (u8)(_Timer2Counter() >> 8);
		case kRegShift:
			_ClearFlags(kIfrShift);
			return fShift;
		case kRegAcr:
			return fAcr;
		case kRegPcr:
			return fPcr;
		case kRegIfr:
			return Ifr();
		case kRegIer:
			return fIer | 0x80;
	}
	return 0xff;
}


void
Via6522::Write(int reg, u8 value)
{
	switch (reg) {
		case kRegOrb:
			_ClearFlags(kIfrCb1 | kIfrCb2);
			fOrb = value;
			if (fPeripheral != nullptr)
				fPeripheral->PortBWritten(PortB());
			break;
		case kRegOra:
			_ClearFlags(kIfrCa1 | kIfrCa2);
			fOra = value;
			break;
		case kRegOraNoHandshake:
			fOra = value;
			break;
		case kRegDdrb:
			fDdrb = value;
			if (fPeripheral != nullptr)
				fPeripheral->PortBWritten(PortB());
			break;
		case kRegDdra:
			fDdra = value;
			break;
		case kRegT1CounterLow:
		case kRegT1LatchLow:
			fTimer1Latch = (fTimer1Latch & 0xff00) | value;
			break;
		case kRegT1CounterHigh:
			fTimer1Latch = (fTimer1Latch & 0x00ff) | (u16)(value << 8);
			_ClearFlags(kIfrTimer1);
			_StartTimer1();
			break;
		case kRegT1LatchHigh:
			fTimer1Latch = (fTimer1Latch & 0x00ff) | (u16)(value << 8);
			_ClearFlags(kIfrTimer1);
			break;
		case kRegT2CounterLow:
			fTimer2LatchLow = value;
			break;
		case kRegT2CounterHigh:
			_ClearFlags(kIfrTimer2);
			fTimer2Start = fScheduler.Now();
			fTimer2Value = (u16)((value << 8) | fTimer2LatchLow);
			fTimer2Armed = true;
			fTimer2->Arm(fTimer2Start + _Ticks(fTimer2Value
				+ kTimerExtraCycles));
			break;
		case kRegShift:
			_ClearFlags(kIfrShift);
			fShift = value;
			break;
		case kRegAcr:
			fAcr = value;
			break;
		case kRegPcr:
			fPcr = value;
			break;
		case kRegIfr:
			_ClearFlags(value & 0x7f);
			break;
		case kRegIer:
			if ((value & 0x80) != 0)
				fIer |= value & 0x7f;
			else
				fIer &= ~(value & 0x7f);
			_UpdateInterrupt();
			break;
	}
}


u32
Via6522::MmioRead(u32 addr, int size, int initiator)
{
	// One byte register per 512 bytes; wider accesses see it in each
	// byte lane they cover.
	u8 value = Read((addr >> 9) & 15);
	PM_LOG(kLogTimer, kLogTrace, "via read %d = %02x", (addr >> 9) & 15,
		value);
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | value;
	return result;
}


void
Via6522::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	// the byte at the register's address
	u8 byte = (u8)(value >> (8 * (size - 1)));
	PM_LOG(kLogTimer, kLogTrace, "via write %d = %02x", (addr >> 9) & 15,
		byte);
	Write((addr >> 9) & 15, byte);
}

}	// namespace powermac
