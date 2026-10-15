/*
 * PowerMac emulator - NCR 53C94 SCSI controller (initiator role), as in
 * Grand Central ("curio", the external bus: 0x10000, DBDMA channel 0).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * The 53C9x family: a 16 byte FIFO, a transfer counter and a command
 * register driving the bus phases (select, transfer information, initiator
 * command complete, message accepted). Registers are a byte every 16;
 * register 14 holds bits 16-23 of the transfer count (53CF94). A DMA
 * transfer moves the data through the DBDMA channel.
 *
 * Targets are ScsiDevices: a command executes in one step once its CDB
 * has arrived. (From BeBox's Hobbit 53C94, with its pseudo-DMA port.)
 */
#ifndef PM_NCR53C94_H
#define PM_NCR53C94_H

#include "Dbdma.h"
#include "../board/Scheduler.h"
#include "../bus/PhysBus.h"
#include "../pci/ScsiDevice.h"
#include "../Types.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace powermac {

class Ncr53c94 : public MmioDevice, public DbdmaChannel::Device {
public:
	typedef std::function<void(bool)> IrqCallback;

								Ncr53c94(Scheduler& scheduler,
									u64 ticksPerSecond, u32 clockHz);
								~Ncr53c94();

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				AttachDevice(int id,
									std::unique_ptr<ScsiDevice> device);
			ScsiDevice*			Device(int id) const
									{ return fDevices[id & 7].get(); }
			void				SetDmaChannel(DbdmaChannel* channel)
									{ fDma = channel; }

			// Registers 0-15.
			u8					Read(int reg);
			void				Write(int reg, u8 value);

	// MmioDevice (offsets within the controller's 4 KB)
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "53c94"; }

	// DbdmaChannel::Device
	virtual	u32					DmaOutput(const u8* data, u32 length);
	virtual	u32					DmaInput(u8* data, u32 length);

private:
	enum Phase {
		kPhaseDataOut = 0,
		kPhaseDataIn = 1,
		kPhaseCommand = 2,
		kPhaseStatus = 3,
		kPhaseMessageOut = 6,
		kPhaseMessageIn = 7,
		kPhaseBusFree = 8
	};

			void				_Command(u8 command);
			void				_Select(bool attention);
			void				_TransferInformation(bool dma);
			void				_InitiatorCommandComplete();
			void				_MessageAccepted();
			void				_ResetBus();
			// The phase after the data phase (or the command phase).
			void				_DataDone();
			void				_RaiseInterrupt(u8 isr, u64 delay);
			void				_DeliverInterrupt();
			void				_UpdateIrq();
			u32					_TransferCount() const;
			// a DMA transfer ended (count or data)
			void				_DmaDone();
			u8					_FifoFlags() const;

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			u32					fClockHz;
			IrqCallback			fIrq;
			DbdmaChannel*		fDma;
			std::unique_ptr<ScsiDevice> fDevices[8];

			std::deque<u8>		fFifo;
			u32					fStartCount;		// STC (24 bits)
			u32					fCount;				// current TC
			bool				fDmaActive;
			u8					fStatus;
			u8					fInterruptStatus;
			u8					fSequenceStep;
			u8					fDestination;
			u8					fTimeout;
			u8					fConfig1;
			u8					fConfig2;
			u8					fConfig3;
			u8					fClockFactor;
			u8					fCommandRegister;

			Phase				fPhase;
			int					fTarget;
			ScsiCommand			fCommand;
			size_t				fDataInPosition;
			std::vector<u8>		fDataOut;

			// interrupt raised later (selection, bus reset)
			u8					fPendingIsr;
			int					fExtraResetInterrupts;
			std::unique_ptr<Timer> fInterruptTimer;
};

}	// namespace powermac

#endif	// PM_NCR53C94_H
