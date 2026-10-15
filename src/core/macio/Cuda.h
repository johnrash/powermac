/*
 * PowerMac emulator - Cuda, the system controller microcontroller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * A Motorola 68HC05 behind the VIA's port B and shift register that runs
 * the ADB, the real time clock, 256 bytes of parameter RAM and the
 * power. Emulated at the protocol level.
 *
 * The handshake (port B: TIP and BYTEACK from the host, TREQ from Cuda,
 * all active low), with the delays of a real Cuda:
 * - TIP high, BYTEACK high: end of a transfer. TREQ goes high; a packet
 *   the host sent is processed and Cuda asserts TREQ to send the reply.
 * - TIP high, BYTEACK low: synchronization. TREQ goes low and pending
 *   data is dropped; at the end of it (TIP and BYTEACK high) autopoll and
 *   the one second packets are off (a system taking the machine over from
 *   another syncs: BeOS after the Mac OS that started it).
 *   Both raise the shift register interrupt (a dummy byte).
 * - TIP low (a transfer), shift register out: each TIP or BYTEACK edge
 *   hands Cuda the byte in the shift register; the interrupt follows.
 * - TIP low, shift register in: each edge shifts the next byte of
 *   Cuda's packet in; with the last one TREQ goes high.
 * With nothing going on, Cuda asserts TREQ and interrupts to send a
 * packet of its own (ADB autopoll data, the one second packets).
 *
 * Packets (first byte: the type): to Cuda 0 = ADB (command, data) and 1
 * = pseudo commands (command, arguments); from Cuda 0 = ADB (status:
 * 0x40 autopoll, 0x02 timeout; command; data), 1 = pseudo command reply
 * (0, command, results), 2 = error (code, type, command), 3 = tick.
 *
 * One second packets (pseudo command 0x1B; Mac OS asks for mode 3 and
 * keeps its clock, the low memory global Time, with them; it never asks
 * for the time otherwise): each time the clock's seconds change, a GET_TIME
 * reply with the time (mode 1; and in modes 2 and 3 the first one, one
 * after a second that had to wait, and every fourth), the reply without
 * the time (mode 2), or the 1-byte tick packet (mode 3). (After
 * DingusPPC's account of Apple's Cuda ERS, facts only.)
 */
#ifndef PM_CUDA_H
#define PM_CUDA_H

#include "AdbDevices.h"
#include "Via6522.h"
#include "../board/Scheduler.h"
#include "../Types.h"

#include <functional>
#include <memory>
#include <vector>

namespace powermac {

class Cuda : public Via6522::Peripheral {
public:
	static const int kPramSize = 256;

								Cuda(Via6522& via, Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Cuda();

			void				Reset();

			AdbBus&				Adb() { return fAdb; }

			// Parameter RAM and the clock's offset: persistent.
			u8*					Pram() { return fPram; }
			bool				IsDirty() const { return fDirty; }
			void				ClearDirty() { fDirty = false; }
			// The real time clock: seconds since 1904 (local time). It
			// runs with the emulated time, from 'time' at 'now'.
			void				SetTime(u32 time);
			u32					Time() const;

			// Power: the system shut down or restarts.
			void				SetPowerCallbacks(
									std::function<void()> powerOff,
									std::function<void()> restart)
									{ fPowerOff = powerOff;
										fRestart = restart; }

			// Wakes the autopolling up after host input.
			void				InputArrived();

	// Via6522::Peripheral
	virtual	void				PortBWritten(u8 value);

private:
			void				_SetTreq(bool asserted);
			void				_ShiftInterruptIn(u64 microseconds,
									bool load, u8 byte = 0);
			void				_ShiftTimerFired();
			void				_ProcessPacket();
			void				_PseudoCommand(const u8* data, int length);
			void				_Reply(u8 type, u8 status, u8 command,
									const u8* data = nullptr,
									int length = 0);
			void				_Error(u8 code, u8 type, u8 command);
			void				_StartSending();
			void				_Poll();
			bool				_Idle() const;
			void				_ArmSecond();
			void				_Second();

			Via6522&			fVia;
			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			AdbBus				fAdb;

			u8					fLastLines;		// TIP | BYTEACK as last seen
			bool				fTreqAsserted;
			bool				fSyncing;
			std::vector<u8>		fInput;			// from the host
			std::vector<u8>		fOutput;		// to the host
			// a packet of ours put off for one of the host's
			std::vector<u8>		fDeferred;
			size_t				fOutputIndex;
			// the reply goes on with zeros (a PRAM read: the host stops)
			bool				fOutputOpenEnded;
			std::unique_ptr<Timer> fShiftTimer;
			bool				fShiftLoad;
			u8					fShiftByte;
			std::unique_ptr<Timer> fTreqTimer;
			std::unique_ptr<Timer> fPollTimer;

			bool				fAutopoll;
			u8					fAutopollRate;	// milliseconds
			u16					fDeviceList;
			u8					fOneSecondMode;
			std::unique_ptr<Timer> fSecondTimer;
			u32					fLastSecond;	// of the last packet
			bool				fSecondFull;	// the next one has the time
			bool				fFileServer;

			u8					fPram[kPramSize];
			bool				fDirty;
			u32					fTimeBase;		// clock at fTimeBaseTicks
			u64					fTimeBaseTicks;

			std::function<void()> fPowerOff;
			std::function<void()> fRestart;
			std::unique_ptr<Timer> fPowerTimer;
			bool				fRestartPending;
};

}	// namespace powermac

#endif	// PM_CUDA_H
