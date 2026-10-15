/*
 * PowerMac emulator - SWIM3, the floppy disk controller, and the SuperDrive.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * In Grand Central at 0x15000 (DBDMA channel 1, interrupt 19). Registers,
 * a byte every 16 (after the register descriptions in Linux's swim3.c):
 *   0x00 data   0x10 timer (counts down at 1 MHz)   0x20 error (read
 *   clears: 0x80 data CRC, 0x40 address CRC, 0x04 overrun, 0x01 underrun)
 *   0x30 mode   0x40 select: CA0-2 and LSTRB (0x08)   0x50 setup (0x80
 *   reset, 0x04 GCR, ...)   0x60 control: writes clear bits (0x80 seek,
 *   0x40 format, 0x20 SEL, 0x10 write, 0x08 action, 0x04/0x02 drive 2/1
 *   enable, 0x01 interrupts)   0x70 status: writes set control bits; reads
 *   0x80/0x40 FIFO, 0x20 error, 0x08 and 0x04 the drive's sense line
 *   (Linux reads bit 3, the Mac OS ROM bit 2), 0x02 interrupt pending   0x80 interrupt (read clears: 0x20 error, 0x10
 *   disk changed, 0x08 transfer done, 0x04 sector seen, 0x02 seek done,
 *   0x01 timer)   0x90 tracks to seek   0xA0 current track (the sector
 *   ID seen last: bit 7 the head, the Mac OS ROM checks it)   0xB0 current
 *   sector   0xC0 gap 3 (read: the format byte of the last sector header,
 *   2 for MFM; the Mac OS driver tells formats by it)   0xD0 first sector
 *   (1-based)   0xE0 sectors to transfer   0xF0 interrupt enable
 *
 * The drive: SEL and CA2-0 pick one of 16 sense lines, read as on the
 * cable (low when active, so inverted) in the status: 0 step direction,
 * 1 stepping, 2 motor on, 3 disk changed (high, unlike the others, once a
 * disk is inserted or ejected until action 12 clears it: the eject latch of
 * DingusPPC's SuperDrive; the other way round Mac OS takes the disk for
 * another one and asks for it back), 4 and 12 read data heads 0 and 1
 * (also the head a transfer uses), 5 one meg drive, 6 single sided, 7 drive
 * present, 8 disk in, 9 write protected, 10 track zero, 11 tachometer, 13
 * GCR mode, 14 seek complete, 15 two meg media. A rising LSTRB does the
 * action of the lines: 0/4 seek direction up/down, 1 step, 2/6 motor on/off,
 * 7 eject, 12 clear disk changed, 9/13 the drive's MFM/GCR mode (line 13).
 * The images are MFM: with the controller in GCR mode (setup 0x04) no
 * sector passes the head. There is one drive: it answers when drive 1 is
 * enabled (control 0x02); for drive 2 the lines stay high, as with nothing
 * on the cable.
 *
 * Writing a sector takes what the controller writes from the DMA: a gap,
 * the sync bytes, the marks (0x99 escapes them: 99 A1 three times, 99 FB,
 * then 99 0F), the 512 data bytes as they are, and the CRC (99 04) and a
 * gap (Linux's swim3.c write_preamble, and Mac OS's alike).
 *
 * Disks (FloppyImage): 720 KB (9 sectors a track) and 1.44 MB (18) MFM
 * disks, 80 cylinders of two sides at 300 rpm; 400 KB (one side) and
 * 800 KB GCR disks, 80 cylinders in five zones of 16 with 12 down to 8
 * sectors and 394 up to 590 rpm, the sectors two apart (0, 6, 1, 7, ...).
 * Sectors take their real time to pass the head (turning all the time, so
 * where a scan finds the head depends on the time); a seek 3 ms a track.
 *
 * GCR (setup 0x04 to read; after MAME's swim3.cpp, BSD-3-Clause, by
 * Olivier Galibert and R. Belmont): the controller finds the address
 * fields (D5 AA 96, then track, sector, side (0x20: head 1, 0x01: track
 * bit 6), format (0x22 two sides, 0x02 one) and their checksum) and, for
 * the sector asked for (0x40 in "first sector": any), hands the DMA the
 * 704 nibbles after the data field's D5 AA AD as their 6-bit values: the
 * sector number, the 524 bytes (12 tag bytes, then the data) encoded three
 * bytes in four nibbles with a running checksum, and the checksum's four.
 * Mac OS decodes them itself. Writes come the same way, after sync bytes
 * and D5 AA AD.
 *
 * Formatting (FORMAT with ACTION) writes a track from the DMA for one
 * revolution. In the stream 0x99 escapes the next byte: a mark (A1, C2) or
 * an address mark (FE, FB, FC), or below 0x10 a control (0x04 writes the
 * CRC). The sectors the ID fields (FE C H R N) name get the bytes of the
 * data field (FB) that follows; TRANSFER_DONE ends the revolution.
 */
#ifndef PM_SWIM3_H
#define PM_SWIM3_H

#include "Dbdma.h"
#include "../board/Scheduler.h"
#include "../bus/PhysBus.h"
#include "../Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace powermac {

class DiskImage;
class FloppyImage;

class Swim3 : public MmioDevice, public DbdmaChannel::Device {
public:
								Swim3(Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~Swim3();

			void				Reset();
			void				SetDmaChannel(DbdmaChannel* channel)
									{ fDma = channel; }
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }

			// A 400 KB, 800 KB, 720 KB or 1.44 MB disk, raw or Disk Copy
			// 4.2; false (with 'error') otherwise.
			bool				InsertDisk(std::unique_ptr<DiskImage> image,
									bool writeProtected, std::string& error);
			void				EjectDisk();
			bool				HasDisk() const { return fDisk != nullptr; }
			// The image in the drive, "" when it is empty.
			std::string			ImagePath() const;

	// MmioDevice: offsets within the controller's 4 KB
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "swim3"; }

	// DbdmaChannel::Device
	virtual	u32					DmaInput(u8* data, u32 length);
	virtual	u32					DmaOutput(const u8* data, u32 length);

private:
			u8					_Read(int reg);
			void				_Write(int reg, u8 value);
			int					_SenseIndex() const;
			bool				_Sense(int line) const;
			u8					_Status() const;
			void				_Strobe();
			void				_ControlChanged(u8 old);
			void				_StartSeek();
			void				_StartAction();
			void				_ActionStep();
			void				_FinishTransfer();
			void				_FinishFormat();
			u32					_TrackBytes() const;
			bool				_IsGcr() const;
			bool				_SectorsPass() const;
			u64					_RevolutionTicks() const;
			u64					_TicksToSlot(int slot) const;
			int					_SlotNow() const;
			int					_SlotSector(int slot) const;
			int					_SectorSlot(int sector) const;
			void				_Raise(u8 bits);
			void				_UpdateInterrupt();
			u64					_SectorTicks() const;
			int					_SectorsPerTrack() const;
			u64					_SectorOffset(int sector) const;
			bool				_LoadSector(int sector);
			bool				_StoreSector(int sector);
			void				_GcrStep();
			bool				_GcrMatches(int sector) const;
			bool				_LoadGcrSector(int sector);
			bool				_StoreGcrSector(int sector,
									const u8* nibbles);
			u32					_GcrWrite(const u8* data, u32 length);

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			DbdmaChannel*		fDma;
			std::function<void(bool)> fInterrupt;
			bool				fInterruptLine;

			// registers
			u8					fError;
			u8					fMode;
			u8					fSelect;
			u8					fSetup;
			u8					fControl;
			u8					fInterruptStatus;
			u8					fInterruptEnable;
			u8					fSeekCount;
			u8					fCurrentTrack;
			u8					fCurrentSector;
			u8					fGap3;
			u8					fHeaderFormat;	// of the last header
			u8					fFirstSector;
			u8					fSectorCount;
			u64					fTimerStart;	// when the timer was set
			u8					fTimerValue;

			// the drive
			std::unique_ptr<FloppyImage> fDisk;
			bool				fWriteProtected;
			bool				fMotorOn;
			bool				fDriveMfm;		// actions 9/13: MFM/GCR
			bool				fDiskChanged;	// line 3; action 12 clears
			int					fCylinder;
			int					fStepDirection;	// +1 or -1
			int					fHead;
			bool				fSeeking;

			// the transfer in progress
			bool				fTransferring;
			bool				fWriting;
			std::vector<u8>		fBuffer;		// one sector (GCR: nibbles)
			size_t				fBufferPosition;
			size_t				fBufferLength;
			int					fSector;		// in fBuffer, from 1 (GCR:
												// its number + 1), 0: none
			bool				fGcrNext;		// GCR: the next sector, any

			// a GCR sector write: the nibbles after D5 AA AD
			u32					fGcrMarks;		// the last bytes, for D5 AA AD
			bool				fGcrData;
			std::vector<u8>		fGcrNibbles;

			// a sector write: the gap, sync and marks before the data
			// (up to the data mark FB and the 99 0F after it), then 512
			// data bytes; the CRC and gap after the last sector are taken
			// too
			enum {
				kWritePreamble, kWriteAfterMark, kWriteMarkEscape,
				kWriteData
			};
			int					fWritePhase;
			bool				fWriteEscape;
			u32					fWriteTrailer;	// bytes still taken

			// a track being formatted: the stream from the DMA (0x99
			// escapes marks and controls)
			bool				fFormatting;
			std::vector<u8>		fFormatStream;
			u32					fFormatBytes;	// on the disk so far
			bool				fFormatEscape;

			std::unique_ptr<Timer> fSeekTimer;
			std::unique_ptr<Timer> fActionTimer;
			std::unique_ptr<Timer> fTimerTimer;
};

}	// namespace powermac

#endif	// PM_SWIM3_H
