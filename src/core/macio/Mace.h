/*
 * PowerMac emulator - MACE, the AMD Am79C940 Ethernet controller.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * In Grand Central at 0x11000, 8-bit registers every 16 bytes; frames move
 * through DBDMA channels 2 (transmit) and 3 (receive), the chip's own
 * interrupt is Grand Central's 14. The registers (Am79C940 data sheet,
 * NetBSD's am79c950reg.h):
 *    0 RCVFIFO   1 XMTFIFO   2 XMTFC (APADXMT 0x01, DXMTFCS 0x08)
 *    3 XMTFS: XMTSV 0x80 (valid; reading clears it), UFLO, LCOL, MORE,
 *      ONE, DEFER, LCAR, RTRY
 *    4 XMTRC   5 RCVFC (ASTRPRCV 0x01)   6 RCVFS (the four status bytes)
 *    7 FIFOFC: frames in the receive (7-4) and transmit (3-0) FIFOs
 *    8 IR (read clears): RCVINT 0x02, XMTINT 0x01, MPCO 0x04, ...
 *    9 IMR (a set bit masks)   10 PR: XMTSV 0x80, TDTREQ 0x40, RDTREQ 0x20
 *   11 BIUCC (SWRST 0x01)   12 FIFOCC   13 MACCC: PROM 0x80, DRCVPA 0x08,
 *      DRCVBC 0x04, ENXMT 0x02, ENRCV 0x01
 *   14 PLSCC   15 PHYCC   16/17 the chip ID (0x0940)
 *   18 IAC: ADDRCHG 0x80, PHYADDR 0x04 and LOGADDR 0x02 restart the byte
 *      pointer of
 *   20 LADRF (the multicast filter, 8 bytes) and 21 PADR (the address, 6)
 *   24 MPC, 26 RNTPC, 27 RCVCC (counters, read clears)   29 UTR
 *
 * Transmit: the DMA fills the frame; the end of an OUTPUT_LAST descriptor
 * ends it. The frame goes to the network backend and, after its time on a
 * 10 Mbit/s wire, XMTFS becomes valid and XMTINT is raised. The chip takes
 * no more data until the driver has read XMTFS.
 *
 * Receive: frames for this station (the address filter: PADR, broadcast,
 * the LADRF hash, or promiscuous) go to the DMA followed by the FCS (left
 * out for 802.3 frames when ASTRPRCV strips them) and the four status bytes
 * RCVCNT, RCVSTS (status bits 7-4, count bits 11-8), RNTPC and RCVCC; the
 * end of the frame ends the INPUT_LAST descriptor. Frames wait in the chip
 * while the DMA is stopped (up to eight; later ones are missed, MPC).
 */
#ifndef PM_MACE_H
#define PM_MACE_H

#include "Dbdma.h"
#include "../board/Scheduler.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace powermac {

class NetworkBackend;

class Mace : public MmioDevice {
public:
								Mace(Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Mace();

			void				Reset();
			void				SetTransmitDma(DbdmaChannel* channel);
			void				SetReceiveDma(DbdmaChannel* channel);
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }
			// The cable: frames go to and come from the backend (none: no
			// network on the other end).
			void				SetBackend(NetworkBackend* backend);

			// A frame from the network (without its FCS); false while the
			// chip cannot take it (it stays queued in the backend).
			bool				Receive(const u8* frame, size_t length);

			// Counters (tests).
			u64					FramesSent() const { return fFramesSent; }
			u64					FramesReceived() const
									{ return fFramesReceived; }

	// MmioDevice: offsets within the chip's 4 KB
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "mace"; }

private:
	class Transmitter : public DbdmaChannel::Device {
	public:
		Transmitter(Mace& mace) : fMace(mace) {}
		virtual u32 DmaOutput(const u8* data, u32 length);
		virtual void DmaOutputLast();
	private:
		Mace& fMace;
	};
	class Receiver : public DbdmaChannel::Device {
	public:
		Receiver(Mace& mace) : fMace(mace) {}
		virtual u32 DmaInput(u8* data, u32 length);
	private:
		Mace& fMace;
	};

			bool				_TransmitReady() const;
			void				_EndFrame();
			void				_TransmitDone();
			bool				_Accepts(const u8* destination) const;
			void				_Missed();
			void				_UpdateInterrupt();
			void				_Poll();

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			std::function<void(bool)> fInterrupt;
			NetworkBackend*		fBackend;
			DbdmaChannel*		fTransmitDma;
			DbdmaChannel*		fReceiveDma;
			Transmitter			fTransmitter;
			Receiver			fReceiver;
			std::unique_ptr<Timer> fTransmitTimer;
			std::unique_ptr<Timer> fPollTimer;

			u8					fTransmitControl;	// XMTFC
			u8					fTransmitStatus;	// XMTFS
			u8					fReceiveControl;	// RCVFC
			u8					fInterruptStatus;	// IR
			u8					fInterruptMask;		// IMR
			u8					fBiuControl;		// BIUCC
			u8					fFifoControl;		// FIFOCC
			u8					fMacControl;		// MACCC
			u8					fPlsControl;		// PLSCC
			u8					fPhyControl;		// PHYCC
			u8					fAddressControl;	// IAC
			u8					fUserTest;			// UTR
			u8					fAddress[6];		// PADR
			u8					fFilter[8];			// LADRF
			int					fAddressIndex;
			int					fFilterIndex;
			u8					fMissed;			// MPC
			u8					fReceiveStatus[4];	// the last RCVFS
			int					fReceiveStatusIndex;

			std::vector<u8>		fFrame;			// being transmitted
			bool				fSending;		// on the wire
			bool				fStatusPending;	// XMTFS not read yet
			std::deque<std::vector<u8> > fReceived;	// for the DMA
			size_t				fReceivePosition;
			u64					fFramesSent;
			u64					fFramesReceived;
};

}	// namespace powermac

#endif	// PM_MACE_H
