/*
 * PowerMac emulator - the ESCC (Z85C30 serial communications controller).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Channel A is the modem port (Open Firmware's console, ttya), channel B
 * the printer port. Asynchronous mode goes to a SerialBackend. The
 * synchronous modes (SDLC: LocalTalk, which AppleTalk opens on the printer
 * port at startup) see an idle network: the receiver hunts for a flag that
 * never comes (RR0 Sync/Hunt set), and a frame sent ends with the Tx
 * underrun/EOM latch set (CRC and flag sent), with the external/status
 * interrupts those changes raise. No DMA. Grand Central decodes it twice:
 *   0x13000 (MacRISC): B control 0x00, B data 0x10, A control 0x20,
 *           A data 0x30
 *   0x12000 (compatible): B control 0, A control 2, B data 4, A data 6
 *
 * A control port write goes to the register WR0 points at (then back to
 * WR0); reads likewise. The interrupt output is the chip's one INT pin.
 */
#ifndef PM_ESCC_H
#define PM_ESCC_H

#include "../board/Scheduler.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <deque>
#include <functional>
#include <memory>

namespace powermac {

class SerialBackend;

class Escc : public MmioDevice {
public:
	// the baud rate generator's clock (RTxC)
	static const u32 kClockHz = 3686400;

								Escc(Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Escc();

			void				Reset();
			void				SetBackend(int channel,
									SerialBackend* backend);
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }

			// Accesses by channel (0 = A, 1 = B) and port.
			u8					ReadControl(int channel);
			void				WriteControl(int channel, u8 value);
			u8					ReadData(int channel);
			void				WriteData(int channel, u8 value);

	// MmioDevice: offsets from 0x12000 (the compatible window, then the
	// MacRISC one at 0x1000).
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "escc"; }

private:
	struct Channel {
		u8					wr[16];
		u8					pointer;
		std::deque<u8>		rx;
		bool				txBusy;			// a byte is being sent
		bool				txFull;			// the buffer holds another
		u8					txBuffer;
		u8					txShift;
		bool				txInterruptPending;
		bool				rxInterruptPending;
		bool				extInterruptPending;
		bool				rxOverrun;
		bool				txUnderrun;		// the Tx underrun/EOM latch
		u8					lastStatus;		// RR0 as last reported
		SerialBackend*		backend;
		std::unique_ptr<Timer> txTimer;
		std::unique_ptr<Timer> pollTimer;
	};

			void				_ResetChannel(int channel, bool hardware);
			void				_WriteRegister(int channel, int reg,
									u8 value);
			u8					_ReadRegister(int channel, int reg);
			u64					_CharacterTicks(int channel) const;
			bool				_Synchronous(int channel) const
									{ return (fChannels[channel].wr[4]
										& 0x0c) == 0; }
			u8					_Status0(int channel);
			// External/status interrupt on a change of an RR0 bit that
			// WR15 enables.
			void				_CheckStatus(int channel);
			void				_StartTransmitter(int channel);
			void				_TransmitDone(int channel);
			void				_Poll(int channel);
			void				_SchedulePoll(int channel);
			void				_UpdateLineConfig(int channel);
			void				_UpdateInterrupt();

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			Channel				fChannels[2];
			std::function<void(bool)> fInterrupt;
			bool				fInterruptLine;
};

}	// namespace powermac

#endif	// PM_ESCC_H
