/*
 * PowerMac emulator - MESH, the Macintosh Enhanced SCSI Hardware.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * The 9500's internal SCSI bus (Grand Central 0x18000, DBDMA channel 10).
 * Registers, one byte every 16:
 *   0x00/0x10 transfer count (low, high)   0x20 FIFO
 *   0x30 sequence: a command (1 arbitrate, 2 select, 3 command, 4 status,
 *        5 data out, 6 data in, 7 message out, 8 message in, 9 bus free,
 *        0x0A-0x0D parity and reselection, 0x0E reset, 0x0F flush FIFO)
 *        with 0x80 DMA, 0x20 ATN
 *   0x40 bus status 0: REQ 0x20, ACK 0x10, ATN 0x08, MSG 0x04, C/D 0x02,
 *        I/O 0x01 (the phase);  0x50 bus status 1: RST 0x80, BSY 0x40,
 *        SEL 0x20
 *   0x60 FIFO count   0x70 exception (0x20/0x10 selected as target,
 *        0x08 reselected, 0x04 arbitration lost, 0x02 phase mismatch,
 *        0x01 selection timeout)   0x80 error (0x40 unexpected
 *        disconnect, 0x20 bus reset, 0x10 sequence error, 0x01 parity)
 *   0x90 interrupt mask, 0xA0 interrupt (0x04 error, 0x02 exception,
 *        0x01 command done; written ones clear)
 *   0xB0 source ID, 0xC0 destination ID, 0xD0 synchronous parameters,
 *   0xE0 MESH ID, 0xF0 selection timeout (10 ms units)
 * (After the register descriptions in NetBSD's mesh.c and Linux's mesh.h.)
 *
 * A command executes in one step once its CDB has arrived; the phases
 * follow as the target would go through them. Without DMA a transfer
 * command moves bytes as the driver fills or drains the FIFO and is done
 * when its count is. The MESH keeps ACK asserted on the last byte of a
 * transfer until the next sequence command, and the target asserts REQ for
 * its next phase after that.
 */
#ifndef PM_MESH_H
#define PM_MESH_H

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

class Mesh : public MmioDevice, public DbdmaChannel::Device {
public:
								Mesh(Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Mesh();

			void				Reset();
			void				SetDmaChannel(DbdmaChannel* channel)
									{ fDma = channel; }
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }
			void				AttachDevice(int id,
									std::unique_ptr<ScsiDevice> device);
			ScsiDevice*			Device(int id) const
									{ return fDevices[id & 7].get(); }

	// MmioDevice (offsets within the controller's 4 KB)
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "mesh"; }

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

			u8					_Read(int reg);
			void				_Write(int reg, u8 value);
			void				_Sequence(u8 value);
			void				_Done(u8 interrupt, u8 exception = 0,
									u8 error = 0, u64 delay = 0);
			void				_Deliver();
			void				_UpdateInterrupt();
			bool				_PhaseIs(Phase phase);
			void				_SetPhase(Phase phase);
			void				_HoldAck();
			void				_ReleaseAck();
			bool				_Requesting() const;
			void				_MessageOut(u8 byte);
			void				_Execute();
			void				_DataDone();
			void				_TransferPio();
			u8					_BusStatus0() const;

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			DbdmaChannel*		fDma;
			std::function<void(bool)> fInterrupt;
			bool				fInterruptLine;
			std::unique_ptr<ScsiDevice> fDevices[8];

			u16					fCount;
			std::deque<u8>		fFifo;
			u8					fSequence;
			u8					fException;
			u8					fError;
			u8					fInterruptMask;
			u8					fInterruptStatus;
			u8					fSourceId;
			u8					fDestinationId;
			u8					fSyncParams;
			u8					fSelectionTimeout;
			bool				fAttention;

			Phase				fPhase;
			u64					fRequestAt;		// when the target asserts REQ
			bool				fAckHeld;		// on a transfer's last byte
			int					fTarget;
			bool				fDmaTransfer;	// a DMA command in progress
			bool				fPioTransfer;	// a FIFO transfer in progress
			ScsiCommand			fCommand;
			std::vector<u8>		fCdb;
			size_t				fDataInPosition;
			std::vector<u8>		fDataOut;
			u8					fMessage;		// the message in to send
			int					fIdentifyLun;	// from IDENTIFY, -1: none

			// completion raised later
			u8					fPendingInterrupt;
			u8					fPendingException;
			u8					fPendingError;
			std::unique_ptr<Timer> fTimer;
};

}	// namespace powermac

#endif	// PM_MESH_H
