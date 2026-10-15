/*
 * PowerMac emulator - SWIM3, the floppy disk controller, and the SuperDrive.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Swim3.h"

#include "../Log.h"
#include "../storage/FloppyImage.h"

#include <algorithm>
#include <cstring>

namespace powermac {

namespace {

enum {
	kRegData = 0, kRegTimer, kRegError, kRegMode, kRegSelect, kRegSetup,
	kRegControl, kRegStatus, kRegInterrupt, kRegSeekCount, kRegTrack,
	kRegSectorNow, kRegGap3, kRegSector, kRegSectorCount,
	kRegInterruptEnable
};

enum {
	kSelectLines = 0x07,
	kSelectStrobe = 0x08
};

enum {
	kControlSeek = 0x80,
	kControlFormat = 0x40,
	kControlSel = 0x20,
	kControlWrite = 0x10,
	kControlAction = 0x08,
	kControlDrive2 = 0x04,
	kControlDrive1 = 0x02,
	kControlInterrupts = 0x01
};

enum {
	kStatusError = 0x20,
	kStatusData = 0x08,
	kStatusSense = 0x04,
	kStatusInterrupt = 0x02
};

enum {
	kIntError = 0x20,
	kIntDiskChanged = 0x10,
	kIntTransferDone = 0x08,
	kIntSectorSeen = 0x04,
	kIntSeekDone = 0x02,
	kIntTimer = 0x01
};

enum {
	kErrorAddressCrc = 0x40
};

enum {
	kSetupGcr = 0x04,			// read GCR
	kSetupNoConversion = 0x10,	// GCR: disk bytes, not their 6-bit values
	kSetupGcrWrite = 0x40
};

// sense lines and actions
enum {
	kLineStepDirection = 0, kLineStepping = 1, kLineMotorOn = 2,
	kLineDiskChanged = 3, kLineReadData0 = 4, kLineOneMegDrive = 5,
	kLineSingleSided = 6, kLineDrivePresent = 7, kLineDiskIn = 8,
	kLineWriteProtect = 9, kLineTrackZero = 10, kLineTacho = 11,
	kLineReadData1 = 12, kLineGcrMode = 13, kLineSeekComplete = 14,
	kLineTwoMegMedia = 15
};

enum {
	kActionSeekUp = 0, kActionStep = 1, kActionMotorOn = 2,
	kActionSeekDown = 4, kActionMotorOff = 6, kActionEject = 7,
	kActionMfm = 9, kActionClearChanged = 12, kActionGcr = 13
};

const int kCylinders = 80;
const int kSectorSize = 512;

// GCR: the disk bytes of the 64 values (Apple's 6-and-2 code), the speed
// of each zone of 16 cylinders, the nibbles of a data field after D5 AA AD
const u8 kGcrBytes[64] = {
	0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
	0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
	0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
	0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
	0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
	0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
	0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};
const int kGcrRpm[5] = { 394, 429, 472, 525, 590 };
const u32 kGcrNibbles = 704;
const u32 kGcrSectorBytes = 524;		// 12 tag bytes and the data


// A disk byte's 6-bit value; -1 if it is none.
int
GcrValue(u8 byte)
{
	for (int i = 0; i < 64; i++) {
		if (kGcrBytes[i] == byte)
			return i;
	}
	return -1;
}


u8
RotateLeft(u8 value)
{
	return (u8)(value << 1 | value >> 7);
}


// The 524 bytes of a sector as 699 nibbles and the 4 of the checksum:
// three bytes a group (the last has two), each mixed with the running
// checksum (three bytes, added with carries from one to the next, the
// third turned left before each group), their top bits in the group's
// first nibble.
void
GcrEncode(const u8* bytes, u8* values)
{
	u8 ca = 0, cb = 0, cc = 0;
	int n = 0;
	for (int i = 0; i < 175; i++) {
		bool last = i == 174;
		u8 va = bytes[3 * i];
		u8 vb = bytes[3 * i + 1];
		u8 vc = last ? 0 : bytes[3 * i + 2];
		cc = RotateLeft(cc);
		u32 sumA = ca + va + (cc & 1);
		ca = (u8)sumA;
		va ^= cc;
		u32 sumB = cb + vb + (sumA >> 8);
		cb = (u8)sumB;
		vb ^= ca;
		if (!last)
			cc = (u8)(cc + vc + (sumB >> 8));
		vc ^= cb;
		values[n++] = ((va >> 2) & 0x30) | ((vb >> 4) & 0x0c)
			| ((vc >> 6) & 0x03);
		values[n++] = va & 0x3f;
		values[n++] = vb & 0x3f;
		if (!last)
			values[n++] = vc & 0x3f;
	}
	values[n++] = ((ca >> 2) & 0x30) | ((cb >> 4) & 0x0c) | ((cc >> 6) & 0x03);
	values[n++] = ca & 0x3f;
	values[n++] = cb & 0x3f;
	values[n++] = cc & 0x3f;
}


// The other way; false if the checksum does not match.
bool
GcrDecode(const u8* values, u8* bytes)
{
	u8 ca = 0, cb = 0, cc = 0;
	int n = 0;
	for (int i = 0; i < 175; i++) {
		bool last = i == 174;
		u8 top = values[n++];
		u8 va = ((top << 2) & 0xc0) | values[n++];
		u8 vb = ((top << 4) & 0xc0) | values[n++];
		u8 vc = last ? 0 : (((top << 6) & 0xc0) | values[n++]);
		cc = RotateLeft(cc);
		va ^= cc;
		u32 sumA = ca + va + (cc & 1);
		ca = (u8)sumA;
		vb ^= ca;
		u32 sumB = cb + vb + (sumA >> 8);
		cb = (u8)sumB;
		bytes[3 * i] = va;
		bytes[3 * i + 1] = vb;
		if (!last) {
			vc ^= cb;
			cc = (u8)(cc + vc + (sumB >> 8));
			bytes[3 * i + 2] = vc;
		}
	}
	u8 top = values[n++];
	return (u8)(((top << 2) & 0xc0) | values[n]) == ca
		&& (u8)(((top << 4) & 0xc0) | values[n + 1]) == cb
		&& (u8)(((top << 6) & 0xc0) | values[n + 2]) == cc;
}

}	// namespace


Swim3::Swim3(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fDma(nullptr),
	fInterruptLine(false),
	fWriteProtected(false),
	fBuffer(kGcrNibbles)
{
	fSeekTimer.reset(new Timer(scheduler, [this]() {
		fCylinder = std::max(0, std::min(kCylinders - 1,
			fCylinder + fStepDirection * fSeekCount));
		fSeekCount = 0;
		fSeeking = false;
		fControl &= ~kControlSeek;
		_Raise(kIntSeekDone);
	}, "swim3-seek"));
	fActionTimer.reset(new Timer(scheduler, [this]() { _ActionStep(); },
		"swim3-action"));
	fTimerTimer.reset(new Timer(scheduler, [this]() {
		fTimerValue = 0;
		_Raise(kIntTimer);
	}, "swim3-timer"));
	Reset();
}


Swim3::~Swim3()
{
}


void
Swim3::Reset()
{
	fError = 0;
	fMode = 0;
	fSelect = 0;
	fSetup = 0;
	fControl = 0;
	fInterruptStatus = 0;
	fInterruptEnable = 0;
	fSeekCount = 0;
	fCurrentTrack = 0;
	fCurrentSector = 0;
	fGap3 = 0;
	fHeaderFormat = 0;
	fFirstSector = 0;
	fSectorCount = 0;
	fTimerStart = 0;
	fTimerValue = 0;
	fMotorOn = false;
	fDriveMfm = false;
	fDiskChanged = fDisk != nullptr;
	fCylinder = 0;
	fStepDirection = 1;
	fHead = 0;
	fSeeking = false;
	fTransferring = false;
	fWriting = false;
	fBufferPosition = 0;
	fBufferLength = kSectorSize;
	fSector = 0;
	fGcrNext = false;
	fGcrMarks = 0;
	fGcrData = false;
	fGcrNibbles.clear();
	fFormatting = false;
	fFormatStream.clear();
	fFormatBytes = 0;
	fFormatEscape = false;
	fWritePhase = kWritePreamble;
	fWriteEscape = false;
	fWriteTrailer = 0;
	fSeekTimer->Cancel();
	fActionTimer->Cancel();
	fTimerTimer->Cancel();
	_UpdateInterrupt();
}


bool
Swim3::InsertDisk(std::unique_ptr<DiskImage> image, bool writeProtected,
	std::string& error)
{
	std::unique_ptr<FloppyImage> floppy = FloppyImage::Wrap(std::move(image),
		error);
	if (!floppy)
		return false;
	if (fDisk)
		fDisk->Flush();
	fDisk = std::move(floppy);
	fWriteProtected = writeProtected || fDisk->IsReadOnly();
	fDiskChanged = true;
	_Raise(kIntDiskChanged);
	return true;
}


void
Swim3::EjectDisk()
{
	if (!fDisk)
		return;
	fDisk->Flush();
	fDisk.reset();
	fDiskChanged = true;
	fTransferring = false;
	fActionTimer->Cancel();
	_Raise(kIntDiskChanged);
}


std::string
Swim3::ImagePath() const
{
	return fDisk ? fDisk->Path() : std::string();
}


bool
Swim3::_IsGcr() const
{
	return fDisk && fDisk->IsGcr();
}


// The sectors on the current cylinder.
int
Swim3::_SectorsPerTrack() const
{
	if (_IsGcr())
		return 12 - std::min(fCylinder / 16, 4);
	return fDisk && fDisk->GetKind() == FloppyImage::kMfm1440 ? 18 : 9;
}


// 300 rpm for MFM; GCR disks turn faster the nearer the center.
u64
Swim3::_RevolutionTicks() const
{
	if (_IsGcr())
		return fTicksPerSecond * 60 / kGcrRpm[std::min(fCylinder / 16, 4)];
	return fTicksPerSecond / 5;
}


// A sector's time under the head.
u64
Swim3::_SectorTicks() const
{
	return _RevolutionTicks() / _SectorsPerTrack();
}


// Sectors come in the disk's coding (setup 0x04: GCR) with the motor on.
bool
Swim3::_SectorsPass() const
{
	if (!fDisk || !fMotorOn)
		return false;
	return _IsGcr() == ((fSetup & kSetupGcr) != 0);
}


// Where a sector (MFM: from 1, GCR: from 0) of the current track and head
// is in the image.
u64
Swim3::_SectorOffset(int sector) const
{
	if (!_IsGcr()) {
		return ((u64)(fCylinder * 2 + fHead) * _SectorsPerTrack()
			+ sector - 1) * kSectorSize;
	}
	u64 before = 0;
	for (int cylinder = 0; cylinder < fCylinder; cylinder++)
		before += (u64)(12 - std::min(cylinder / 16, 4)) * fDisk->Sides();
	return (before + (u64)fHead * _SectorsPerTrack() + sector) * kSectorSize;
}


int
Swim3::_SenseIndex() const
{
	return ((fControl & kControlSel) != 0 ? 8 : 0) | (fSelect & kSelectLines);
}


bool
Swim3::_Sense(int line) const
{
	switch (line) {
		case kLineStepDirection:
			return fStepDirection < 0;
		case kLineMotorOn:
			return fMotorOn;
		case kLineDiskChanged:
			return !fDiskChanged;		// high while latched
		case kLineDrivePresent:
			return true;
		case kLineDiskIn:
			return fDisk != nullptr;
		case kLineWriteProtect:
			return fDisk && fWriteProtected;
		case kLineTrackZero:
			return fCylinder == 0;
		case kLineTacho:
			// pulses while the motor turns
			return fMotorOn && (fScheduler.Now() * 120 / fTicksPerSecond)
				% 2 != 0;
		case kLineGcrMode:
			return !fDriveMfm;
		case kLineSeekComplete:
			return !fSeeking;
		case kLineTwoMegMedia:
			return fDisk && fDisk->GetKind() == FloppyImage::kMfm1440;
	}
	return false;
}


u8
Swim3::_Status() const
{
	u8 value = 0;
	// the drive's sense line as it is on the cable (low when active): Linux
	// reads it in bit 3, the Mac OS ROM in bit 2 (its drive type routine
	// at 0xFFCFA1B8 reads lines 15, 7, 6 and 5 there: a SuperDrive). Only
	// the first drive is there: with the second one enabled the lines stay
	// high (Mac OS polls both for a disk).
	if ((fControl & kControlDrive1) == 0 || !_Sense(_SenseIndex()))
		value |= kStatusData | kStatusSense;
	if ((fInterruptStatus & fInterruptEnable) != 0)
		value |= kStatusInterrupt;
	if (fError != 0)
		value |= kStatusError;
	return value;
}


void
Swim3::_Strobe()
{
	int line = _SenseIndex();
	if ((fControl & kControlDrive1) == 0) {
		PM_LOG(kLogFloppy, kLogDebug, "swim3: action %d for no drive", line);
		return;
	}
	switch (line) {
		case kActionSeekUp:
			fStepDirection = 1;
			break;
		case kActionSeekDown:
			fStepDirection = -1;
			break;
		case kActionStep:
			fCylinder = std::max(0, std::min(kCylinders - 1,
				fCylinder + fStepDirection));
			break;
		case kActionMotorOn:
			fMotorOn = true;
			break;
		case kActionMotorOff:
			fMotorOn = false;
			break;
		case kActionEject:
			EjectDisk();
			break;
		case kActionMfm:
			fDriveMfm = true;
			break;
		case kActionGcr:
			fDriveMfm = false;
			break;
		case kActionClearChanged:
			fDiskChanged = false;
			break;
	}
	PM_LOG(kLogFloppy, kLogDebug, "swim3: action %d (cylinder %d, motor %d) "
		"at %.6f s", line, fCylinder, fMotorOn,
		(double)fScheduler.Now() / fTicksPerSecond);
}


void
Swim3::_ControlChanged(u8 old)
{
	u8 set = fControl & ~old;
	if ((set & kControlSeek) != 0)
		_StartSeek();
	if ((set & kControlAction) != 0)
		_StartAction();
	if ((old & kControlAction) != 0 && (fControl & kControlAction) == 0) {
		// stopped
		fActionTimer->Cancel();
		fTransferring = false;
		fFormatting = false;
	}
	_UpdateInterrupt();
}


void
Swim3::_StartSeek()
{
	fSeeking = true;
	u64 ticks = fTicksPerSecond * 3 / 1000 * std::max<int>(1, fSeekCount);
	fSeekTimer->ArmIn(ticks);
	PM_LOG(kLogFloppy, kLogDebug, "swim3: seek %d tracks %s from %d",
		fSeekCount, fStepDirection > 0 ? "up" : "down", fCylinder);
}


// Action: with sectors to transfer, a read or write of them on the
// current track and head; without, the sector headers passing by.
void
Swim3::_StartAction()
{
	fHead = _SenseIndex() == kLineReadData1 ? 1 : 0;
	if ((fControl & kControlFormat) != 0) {
		// a whole track, index to index
		fFormatting = true;
		fFormatStream.clear();
		fFormatBytes = 0;
		fFormatEscape = false;
		fTransferring = false;
		PM_LOG(kLogFloppy, kLogDebug, "swim3: format cylinder %d head %d "
			"(setup %02x)", fCylinder, fHead, fSetup);
		// the disk's coding: GCR tracks on a GCR disk
		if (!fDisk || !fMotorOn
			|| _IsGcr() != ((fSetup & (kSetupGcr | kSetupGcrWrite)) != 0))
			return;
		if (_IsGcr()) {
			// from the index for a little more than a revolution: the
			// first header after it is sector 0's (Mac OS checks that the
			// end of the track left it, else it shortens the gaps)
			u64 sectorTicks = _SectorTicks();
			u64 revolution = sectorTicks * _SectorsPerTrack();
			u64 now = fScheduler.Now();
			fActionTimer->ArmIn((now / revolution + 2) * revolution
				- sectorTicks / 2 - now);
		} else
			fActionTimer->ArmIn(_RevolutionTicks());
		if (fDma != nullptr)
			fDma->Kick();
		return;
	}
	fWriting = (fControl & kControlWrite) != 0;
	fTransferring = fSectorCount > 0;
	fWritePhase = kWritePreamble;
	fWriteEscape = false;
	fWriteTrailer = 0;
	fSector = 0;
	fBufferPosition = 0;
	fGcrNext = false;
	PM_LOG(kLogFloppy, kLogDebug, "swim3: action: %s %d sectors from %d, "
		"cylinder %d head %d%s", fWriting ? "write" : fTransferring ? "read"
			: "scan", fSectorCount, fFirstSector, fCylinder, fHead,
		(fSetup & kSetupGcr) != 0 ? " (GCR)" : "");
	if (!_SectorsPass())
		return;		// nothing (or none of this coding) passes the head
	int slot = -1;
	if (fTransferring && !_IsGcr() && fFirstSector >= 1
		&& fFirstSector <= _SectorsPerTrack())
		slot = fFirstSector - 1;
	else if (fTransferring && _IsGcr() && (fFirstSector & 0xc0) == 0)
		slot = _SectorSlot(fFirstSector & 0x3f);
	fActionTimer->ArmIn(_TicksToSlot(slot));
}


// The disk turns all the time: a sector header passes the head every
// sector time, the first slot of the track at the multiples of a
// revolution. The time until 'slot' starts, or with -1 until the next one
// does.
u64
Swim3::_TicksToSlot(int slot) const
{
	u64 sectorTicks = _SectorTicks();
	u64 now = fScheduler.Now();
	u64 slots = now / sectorTicks;
	int sectors = _SectorsPerTrack();
	int current = (int)(slots % sectors);
	int ahead = slot < 0 ? 1 : (slot - current - 1 + 2 * sectors) % sectors + 1;
	return (slots + ahead) * sectorTicks - now;
}


// The slot starting now (at a sector time boundary).
int
Swim3::_SlotNow() const
{
	u64 sectorTicks = _SectorTicks();
	u64 slots = (fScheduler.Now() + sectorTicks / 2) / sectorTicks;
	return (int)(slots % _SectorsPerTrack());
}


// The sector in a slot: MFM sectors one after the other from 1, GCR ones
// from 0 two slots apart (0, 6, 1, 7, ... with 12).
int
Swim3::_SlotSector(int slot) const
{
	if (!_IsGcr())
		return slot + 1;
	int sectors = _SectorsPerTrack();
	int position = 0;
	for (int sector = 0; sector < sectors; sector++) {
		if (position == slot)
			return sector;
		position = (position + 2) % sectors;
		if (position == 0)
			position = 1;
	}
	return 0;
}


// The other way; -1 if the track has no such sector.
int
Swim3::_SectorSlot(int sector) const
{
	for (int slot = 0; slot < _SectorsPerTrack(); slot++) {
		if (_SlotSector(slot) == sector)
			return slot;
	}
	return -1;
}


// The next sector comes under the head.
void
Swim3::_ActionStep()
{
	if ((fControl & kControlAction) == 0 || !fDisk)
		return;
	if (fFormatting) {
		_FinishFormat();
		return;
	}
	if (_IsGcr()) {
		_GcrStep();
		return;
	}
	int sectors = _SectorsPerTrack();
	fCurrentTrack = (u8)(fHead << 7 | fCylinder);
	fHeaderFormat = 2;		// MFM, 512 bytes
	if (!fTransferring) {
		fCurrentSector = (u8)_SlotSector(_SlotNow());
		_Raise(kIntSectorSeen);
		fActionTimer->ArmIn(_TicksToSlot(-1));
		return;
	}
	if (fFirstSector < 1 || fFirstSector > sectors) {
		// no such sector on the track
		fError |= kErrorAddressCrc;
		fTransferring = false;
		_Raise(kIntError);
		return;
	}
	fCurrentSector = fFirstSector;
	fSector = fFirstSector;
	fBufferPosition = 0;
	fBufferLength = kSectorSize;
	if (!fWriting && !_LoadSector(fSector)) {
		fError |= kErrorAddressCrc;
		fTransferring = false;
		_Raise(kIntError);
		return;
	}
	if (fDma != nullptr)
		fDma->Kick();
}


bool
Swim3::_LoadSector(int sector)
{
	return fDisk->Read(_SectorOffset(sector), fBuffer.data(), kSectorSize);
}


bool
Swim3::_StoreSector(int sector)
{
	if (fWriteProtected)
		return false;
	return fDisk->Write(_SectorOffset(sector), fBuffer.data(), kSectorSize);
}


// A GCR sector header passes the head: with a transfer, the sector asked
// for gets its data field read or written.
void
Swim3::_GcrStep()
{
	int sector = _SlotSector(_SlotNow());
	fCurrentTrack = (u8)(fHead << 7 | fCylinder);
	fCurrentSector = (u8)sector;
	fHeaderFormat = fDisk->Sides() == 2 ? 0x22 : 0x02;
	if (!fTransferring) {
		_Raise(kIntSectorSeen);
		fActionTimer->ArmIn(_TicksToSlot(-1));
		return;
	}
	if ((fFirstSector & 0xc0) == 0 && !fGcrNext
		&& _SectorSlot(fFirstSector & 0x3f) < 0) {
		// no such sector on the track
		fError |= kErrorAddressCrc;
		fTransferring = false;
		_Raise(kIntError);
		return;
	}
	if (!_GcrMatches(sector)) {
		fActionTimer->ArmIn(_TicksToSlot(-1));
		return;
	}
	fGcrNext = false;
	fSector = sector + 1;
	fBufferPosition = 0;
	if (fWriting) {
		fGcrMarks = 0;
		fGcrData = false;
		fGcrNibbles.clear();
	} else if (!_LoadGcrSector(sector)) {
		fError |= kErrorAddressCrc;
		fTransferring = false;
		_Raise(kIntError);
		return;
	}
	if (fDma != nullptr)
		fDma->Kick();
}


// The sector asked for ("first sector": 0x40 any, 0x80 none), or after a
// sector the next one of a transfer of several.
bool
Swim3::_GcrMatches(int sector) const
{
	if (fGcrNext)
		return true;
	if ((fFirstSector & 0x80) != 0)
		return false;
	return (fFirstSector & 0x40) != 0 || (fFirstSector & 0x3f) == sector;
}


// The data field of a sector: its number and the encoded 524 bytes, as
// 6-bit values (or as disk bytes without conversion).
bool
Swim3::_LoadGcrSector(int sector)
{
	u8 bytes[kGcrSectorBytes];
	u64 offset = _SectorOffset(sector);
	if (!fDisk->ReadTag((u32)(offset / kSectorSize), bytes)
		|| !fDisk->Read(offset, bytes + FloppyImage::kTagSize, kSectorSize))
		return false;
	fBuffer[0] = (u8)sector;
	GcrEncode(bytes, fBuffer.data() + 1);
	if ((fSetup & kSetupNoConversion) != 0) {
		for (u32 i = 0; i < kGcrNibbles; i++)
			fBuffer[i] = kGcrBytes[fBuffer[i] & 0x3f];
	}
	fBufferLength = kGcrNibbles;
	return true;
}


// A data field written (the 704 nibbles after D5 AA AD, as values).
bool
Swim3::_StoreGcrSector(int sector, const u8* nibbles)
{
	if (fWriteProtected)
		return false;
	u8 bytes[kGcrSectorBytes];
	if (!GcrDecode(nibbles + 1, bytes)) {
		PM_LOG(kLogFloppy, kLogWarning, "swim3: GCR sector %d (cylinder %d "
			"head %d) written with a bad checksum", sector, fCylinder,
			fHead);
		return false;
	}
	u64 offset = _SectorOffset(sector);
	return fDisk->Write(offset, bytes + FloppyImage::kTagSize, kSectorSize)
		&& fDisk->WriteTag((u32)(offset / kSectorSize), bytes);
}


// A sector went through DMA: the next one, or the transfer is done.
void
Swim3::_FinishTransfer()
{
	if (_IsGcr()) {
		// the next data field to pass, whichever sector
		fSectorCount--;
		fBufferPosition = 0;
		fSector = 0;
		if (fSectorCount == 0) {
			fTransferring = false;
			_Raise(kIntTransferDone);
			return;
		}
		fGcrNext = true;
		fActionTimer->ArmIn(_TicksToSlot(-1));
		return;
	}
	fFirstSector++;
	fSectorCount--;
	fBufferPosition = 0;
	fSector = 0;
	if (fSectorCount == 0) {
		fTransferring = false;
		_Raise(kIntTransferDone);
		return;
	}
	fActionTimer->ArmIn(_TicksToSlot(fFirstSector >= 1
		&& fFirstSector <= _SectorsPerTrack() ? fFirstSector - 1 : -1));
}


u32
Swim3::DmaInput(u8* data, u32 length)
{
	if (!fTransferring || fWriting || fSector == 0)
		return 0;
	u32 given = (u32)std::min<size_t>(length,
		fBufferLength - fBufferPosition);
	memcpy(data, fBuffer.data() + fBufferPosition, given);
	fBufferPosition += given;
	if (fBufferPosition == fBufferLength)
		_FinishTransfer();
	return given;
}


// The bytes on a track: 500 kbit/s (1.44 MB) or 250 kbit/s at 300 rpm;
// GCR at 2 µs a bit cell at the zone's speed.
u32
Swim3::_TrackBytes() const
{
	if (_IsGcr())
		return (u32)(30318342 / kGcrRpm[std::min(fCylinder / 16, 4)] / 8);
	return _SectorsPerTrack() == 18 ? 12500 : 6250;
}


// The revolution is over: the sectors of the track get the data fields.
void
Swim3::_FinishFormat()
{
	fFormatting = false;
	fCurrentTrack = (u8)(fHead << 7 | fCylinder);
	if (fWriteProtected) {
		fError |= kErrorAddressCrc;
		_Raise(kIntError);
		return;
	}
	// the stream as disk bytes: marks (escaped bytes from 0x10 up) and data
	struct Byte {
		u8 value;
		bool mark;
	};
	std::vector<Byte> bytes;
	bool escape = false;
	for (u8 value : fFormatStream) {
		if (escape) {
			escape = false;
			if (value >= 0x10)
				bytes.push_back({ value, true });
		} else if (value == 0x99)
			escape = true;
		else
			bytes.push_back({ value, _IsGcr() });
	}
	int sectors = _SectorsPerTrack();
	int sector = 0;
	int written = 0;
	if (_IsGcr()) {
		// address fields (D5 AA 96, then track, sector, side, format and
		// checksum) and the data fields after them (D5 AA AD, 704 nibbles).
		// Bytes below 0x40 are values the controller encodes (Mac OS gives
		// 96 and AD so, as 00 and 0B), the others disk bytes (D5, AA; the
		// self-sync bytes with their top bit clear)
		auto value = [](u8 byte) {
			int v = byte < 0x40 ? byte : GcrValue(byte);
			return (u8)(v < 0 ? 0 : v);
		};
		sector = -1;
		for (size_t i = 0; i + 2 < bytes.size(); i++) {
			if (bytes[i].value != 0xd5 || bytes[i + 1].value != 0xaa)
				continue;
			if ((bytes[i + 2].value == 0x96 || bytes[i + 2].value == 0x00)
				&& i + 7 < bytes.size()) {
				sector = value(bytes[i + 4].value);
				i += 7;
			} else if ((bytes[i + 2].value == 0xad
					|| bytes[i + 2].value == 0x0b)
				&& sector >= 0 && sector < sectors
				&& i + 3 + kGcrNibbles <= bytes.size()) {
				u8 nibbles[kGcrNibbles];
				for (u32 n = 0; n < kGcrNibbles; n++)
					nibbles[n] = value(bytes[i + 3 + n].value);
				if (!_StoreGcrSector(sector, nibbles)) {
					fError |= kErrorAddressCrc;
					_Raise(kIntError);
					return;
				}
				written++;
				sector = -1;
				i += 2 + kGcrNibbles;
			}
		}
		PM_LOG(kLogFloppy, kLogDebug, "swim3: formatted %d GCR sectors "
			"(cylinder %d head %d)", written, fCylinder, fHead);
		fFormatStream.clear();
		_Raise(kIntTransferDone);
		return;
	}
	for (size_t i = 0; i < bytes.size(); i++) {
		if (!bytes[i].mark)
			continue;
		if (bytes[i].value == 0xfe && i + 4 < bytes.size()) {
			sector = bytes[i + 3].value;		// C H R N
			i += 4;
		} else if (bytes[i].value == 0xfb && sector >= 1
			&& sector <= sectors) {
			size_t count = 0;
			for (size_t j = i + 1; j < bytes.size() && count < kSectorSize
					&& !bytes[j].mark; j++)
				fBuffer[count++] = bytes[j].value;
			if (count == 0)
				continue;
			for (; count < kSectorSize; count++)
				fBuffer[count] = fBuffer[count - 1];
			if (!_StoreSector(sector)) {
				fError |= kErrorAddressCrc;
				_Raise(kIntError);
				return;
			}
			written++;
			sector = 0;
		}
	}
	PM_LOG(kLogFloppy, kLogDebug, "swim3: formatted %d sectors (cylinder %d "
		"head %d)", written, fCylinder, fHead);
	fFormatStream.clear();
	_Raise(kIntTransferDone);
}


u32
Swim3::DmaOutput(const u8* data, u32 length)
{
	if (fFormatting) {
		// one revolution's bytes; the rest waits for the next format. Mac
		// OS's GCR tracks are longer (a long gap at the start, which the
		// end of the track writes over): up to two
		u32 limit = _TrackBytes() * (_IsGcr() ? 2 : 1);
		u32 taken = 0;
		while (taken < length && fFormatBytes < limit) {
			u8 value = data[taken++];
			fFormatStream.push_back(value);
			if (fFormatEscape) {
				fFormatEscape = false;
				fFormatBytes += value == 0x04 ? 2 : value >= 0x10 ? 1 : 0;
			} else if (value == 0x99)
				fFormatEscape = true;
			else
				fFormatBytes++;
		}
		return taken;
	}
	if (!fTransferring && fWriting && fWriteTrailer > 0) {
		// the CRC and gap after the last sector
		u32 taken = std::min(length, fWriteTrailer);
		fWriteTrailer -= taken;
		return taken;
	}
	if (!fTransferring || !fWriting || fSector == 0)
		return 0;
	if (_IsGcr())
		return _GcrWrite(data, length);
	u32 taken = 0;
	while (taken < length && fTransferring && fSector != 0) {
		u8 value = data[taken++];
		switch (fWritePhase) {
			case kWritePreamble:
				// up to the data mark
				if (fWriteEscape) {
					fWriteEscape = false;
					if (value == 0xfb)
						fWritePhase = kWriteAfterMark;
				} else if (value == 0x99)
					fWriteEscape = true;
				break;
			case kWriteAfterMark:
				// the 99 0F after the data mark, then the data
				if (value == 0x99) {
					fWritePhase = kWriteMarkEscape;
					break;
				}
				fWritePhase = kWriteData;
				fBuffer[fBufferPosition++] = value;
				break;
			case kWriteMarkEscape:
				fWritePhase = kWriteData;
				if (value == 0x0f)
					break;
				// data that starts with 0x99
				fBuffer[fBufferPosition++] = 0x99;
				fBuffer[fBufferPosition++] = value;
				break;
			case kWriteData:
				fBuffer[fBufferPosition++] = value;
				break;
		}
		if (fWritePhase == kWriteData && fBufferPosition >= kSectorSize) {
			fWritePhase = kWritePreamble;
			PM_LOG(kLogFloppy, kLogDebug, "swim3: sector %d (cylinder %d head "
				"%d) written, data %02x %02x %02x %02x", fSector, fCylinder,
				fHead, fBuffer[0], fBuffer[1], fBuffer[2], fBuffer[3]);
			if (!_StoreSector(fSector)) {
				fError |= kErrorAddressCrc;
				fTransferring = false;
				_Raise(kIntError);
				return taken;
			}
			_FinishTransfer();
			if (!fTransferring)
				fWriteTrailer = 64;
		}
	}
	return taken;
}


// A GCR sector written: sync bytes up to the data mark (D5 AA AD), then
// the sector number and the 703 nibbles of the data and checksum, as 6-bit
// values (or disk bytes). 0x99 escapes a control byte (0x08: the end).
u32
Swim3::_GcrWrite(const u8* data, u32 length)
{
	u32 taken = 0;
	while (taken < length && fTransferring && fSector != 0) {
		u8 value = data[taken++];
		if (fWriteEscape) {
			fWriteEscape = false;
			continue;
		}
		if (value == 0x99) {
			fWriteEscape = true;
			continue;
		}
		if (!fGcrData) {
			fGcrMarks = (fGcrMarks << 8 | value) & 0xffffff;
			if (fGcrMarks == 0xd5aaad || fGcrMarks == 0xd5aa0b) {
				fGcrData = true;
				fGcrNibbles.clear();
			}
			continue;
		}
		int nibble = value < 0x40 ? value : GcrValue(value);
		fGcrNibbles.push_back((u8)(nibble < 0 ? 0 : nibble));
		if (fGcrNibbles.size() < kGcrNibbles)
			continue;
		int sector = fSector - 1;
		PM_LOG(kLogFloppy, kLogDebug, "swim3: GCR sector %d (cylinder %d "
			"head %d) written", sector, fCylinder, fHead);
		if (!_StoreGcrSector(sector, fGcrNibbles.data())) {
			fError |= kErrorAddressCrc;
			fTransferring = false;
			_Raise(kIntError);
			return taken;
		}
		fGcrData = false;
		fGcrMarks = 0;
		_FinishTransfer();
		if (!fTransferring)
			fWriteTrailer = 64;
	}
	return taken;
}


void
Swim3::_Raise(u8 bits)
{
	fInterruptStatus |= bits;
	_UpdateInterrupt();
}


void
Swim3::_UpdateInterrupt()
{
	bool line = (fControl & kControlInterrupts) != 0
		&& (fInterruptStatus & fInterruptEnable) != 0;
	if (line == fInterruptLine)
		return;
	fInterruptLine = line;
	if (fInterrupt)
		fInterrupt(line);
}


u8
Swim3::_Read(int reg)
{
	switch (reg) {
		case kRegTimer:
		{
			if (fTimerValue == 0)
				return 0;
			u64 elapsed = (fScheduler.Now() - fTimerStart) * 1000000
				/ fTicksPerSecond;
			return elapsed >= fTimerValue ? 0 : (u8)(fTimerValue - elapsed);
		}
		case kRegError:
		{
			u8 value = fError;
			fError = 0;
			return value;
		}
		case kRegMode: return fMode;
		case kRegSelect: return fSelect;
		case kRegSetup: return fSetup;
		case kRegControl: return fControl;
		case kRegStatus: return _Status();
		case kRegInterrupt:
		{
			u8 value = fInterruptStatus;
			fInterruptStatus = 0;
			_UpdateInterrupt();
			return value;
		}
		case kRegSeekCount: return fSeekCount;
		case kRegTrack: return fCurrentTrack;
		case kRegSectorNow: return fCurrentSector;
		// read: the format byte of the sector header seen last (the size
		// code N of an MFM header); Mac OS tells disk formats by it
		case kRegGap3: return fHeaderFormat;
		case kRegSector: return fFirstSector;
		case kRegSectorCount: return fSectorCount;
		case kRegInterruptEnable: return fInterruptEnable;
	}
	return 0;
}


void
Swim3::_Write(int reg, u8 value)
{
	switch (reg) {
		case kRegTimer:
			fTimerValue = value;
			fTimerStart = fScheduler.Now();
			fTimerTimer->Cancel();
			if (value != 0)
				fTimerTimer->ArmIn(fTicksPerSecond / 1000000 * value);
			break;
		case kRegMode:
			fMode = value;
			break;
		case kRegSelect:
		{
			u8 old = fSelect;
			fSelect = value;
			if ((old & kSelectStrobe) == 0 && (value & kSelectStrobe) != 0)
				_Strobe();
			break;
		}
		case kRegSetup:
			fSetup = value;
			break;
		case kRegControl:
		{
			// written ones clear control bits
			u8 old = fControl;
			fControl &= ~value;
			_ControlChanged(old);
			break;
		}
		case kRegStatus:
		{
			// written ones set control bits
			u8 old = fControl;
			fControl |= value;
			_ControlChanged(old);
			break;
		}
		case kRegSeekCount:
			fSeekCount = value;
			break;
		case kRegGap3:
			fGap3 = value;
			break;
		case kRegSector:
			fFirstSector = value;
			break;
		case kRegSectorCount:
			fSectorCount = value;
			break;
		case kRegInterruptEnable:
			fInterruptEnable = value;
			_UpdateInterrupt();
			break;
	}
}


u32
Swim3::MmioRead(u32 addr, int size, int initiator)
{
	int reg = (addr >> 4) & 15;
	u8 value = _Read(reg);
	PM_LOG(kLogFloppy, kLogTrace, "swim3: read %x = %02x", reg, value);
	u32 result = 0;
	for (int i = 0; i < size; i++)
		result = (result << 8) | value;
	return result;
}


void
Swim3::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	int reg = (addr >> 4) & 15;
	u8 byte = (u8)(value >> (8 * (size - 1)));
	PM_LOG(kLogFloppy, kLogTrace, "swim3: write %x = %02x", reg, byte);
	_Write(reg, byte);
}

}	// namespace powermac
