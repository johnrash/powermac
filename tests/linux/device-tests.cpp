/*
 * Device unit tests: the behaviours Mac OS's drivers depend on that are
 * easy to break and slow to find again by booting.
 *  - MESH: PIO transfers driven by the FIFO, ACK held on a transfer's last
 *    byte (REQ for the next phase only after the next sequence command),
 *    the wait for bus free ending with a phase mismatch, clearing the
 *    exception register through the interrupt register, DMA data in.
 *  - SCSI disk: the Apple vendor mode page Drive Setup checks.
 *  - Twin Turbo 128: the vertical blank interrupt (SSTATUS 0x10, STGCTL
 *    0x400) the Mac OS driver loads its CLUT from.
 *  - Twin Turbo 128 drawing engine: fills and copies, the address unit.
 *  - SWIM3: the drive's sense lines, seeks, reading sectors by DMA.
 *  - AWACS: DMA output played at the sample rate, attenuation.
 *  - ESCC: SDLC (LocalTalk) on an idle network: the receiver hunts, a frame
 *    ends with the Tx underrun/EOM latch and an external/status interrupt.
 *  - ADB keyboard: keys held across a reset or a flush are reported again.
 *  - Key codes: ADB codes for names and ASCII.
 *
 * usage: device-tests
 */
#include "core/Log.h"
#include "core/board/Scheduler.h"
#include "core/bus/PhysBus.h"
#include "core/host/KeyCodes.h"
#include "core/host/MacTime.h"
#include "core/host/AudioSink.h"
#include "core/host/NetworkBackend.h"
#include "core/host/VideoSink.h"
#include "core/macio/AdbDevices.h"
#include "core/macio/Awacs.h"
#include "core/macio/Cuda.h"
#include "core/macio/Dbdma.h"
#include "core/macio/Escc.h"
#include "core/macio/GrandCentral.h"
#include "core/macio/Mace.h"
#include "core/macio/Mesh.h"
#include "core/macio/Swim3.h"
#include "core/pci/ScsiDevice.h"
#include "core/storage/DiskImage.h"
#include "core/storage/FloppyImage.h"
#include "core/video/ImsTwinTurbo128.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace powermac;

static int sFailures = 0;
static int sChecks = 0;


static void
Expect(bool condition, const char* what)
{
	sChecks++;
	if (!condition) {
		sFailures++;
		printf("FAIL: %s\n", what);
	}
}


static const u64 kTicksPerSecond = 540000000;	// 45 MHz bus * 12


static u64
Microseconds(u64 us)
{
	return kTicksPerSecond / 1000000 * us;
}


// Emulated time for the devices: advances the clock and fires due timers.
struct Clock {
	Scheduler scheduler;
	u64 now = 0;

	void Advance(u64 ticks)
	{
		u64 end = now + ticks;
		while (true) {
			u64 next = scheduler.NextEventTime();
			if (next > end)
				break;
			now = next;
			scheduler.SetNow(now);
			scheduler.RunDue(now);
		}
		now = end;
		scheduler.SetNow(now);
	}
};


class MemoryImage : public DiskImage {
public:
	explicit MemoryImage(u64 size) : fData(size, 0) {}

	virtual u64 Size() const { return fData.size(); }
	virtual bool IsReadOnly() const { return false; }
	virtual bool Read(u64 offset, void* buffer, size_t length)
	{
		if (offset + length > fData.size())
			return false;
		memcpy(buffer, &fData[offset], length);
		return true;
	}
	virtual bool Write(u64 offset, const void* buffer, size_t length)
	{
		if (offset + length > fData.size())
			return false;
		memcpy(&fData[offset], buffer, length);
		return true;
	}
	virtual std::string Path() const { return "memory"; }
	virtual const char* Format() const { return "memory"; }

	std::vector<u8> fData;
};


// #pragma mark - MESH


enum {
	kMeshCountLow = 0x00, kMeshCountHigh = 0x10, kMeshFifo = 0x20,
	kMeshSequence = 0x30, kMeshBusStatus0 = 0x40, kMeshBusStatus1 = 0x50,
	kMeshFifoCount = 0x60, kMeshException = 0x70, kMeshError = 0x80,
	kMeshInterruptMask = 0x90, kMeshInterrupt = 0xa0,
	kMeshDestinationId = 0xc0
};

enum {
	kBs0Req = 0x20, kBs0Ack = 0x10, kPhaseMask = 0x07,
	kPhaseDataIn = 1, kPhaseCommand = 2, kPhaseStatus = 3,
	kPhaseMessageIn = 7,
	kIntDone = 0x01, kIntException = 0x02,
	kExcPhaseMismatch = 0x02, kExcSelectionTimeout = 0x01
};


struct MeshRig {
	Clock clock;
	PhysBus bus;
	std::vector<u8> ram;
	Mesh mesh;
	DbdmaChannel dma;
	MemoryImage* image;
	bool line = false;

	MeshRig()
		:
		ram(0x10000, 0),
		mesh(clock.scheduler, kTicksPerSecond),
		dma(bus, "mesh-dma")
	{
		bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
		std::unique_ptr<MemoryImage> disk(new MemoryImage(1 << 20));
		image = disk.get();
		for (size_t i = 0; i < 512; i++)
			disk->fData[512 + i] = (u8)(i * 7 + 1);	// block 1
		mesh.AttachDevice(0, std::unique_ptr<ScsiDevice>(new ScsiDisk(
			std::move(disk), "QUANTUM", "FIREBALL1080S")));
		mesh.SetDmaChannel(&dma);
		dma.SetDevice(&mesh);
		mesh.SetInterruptCallback([this](bool level) { line = level; });
		Write(kMeshInterruptMask, 0x07);
		// the reset unit attention
		Command({ 0x00, 0, 0, 0, 0, 0 });
		Status();
	}

	u8 Read(u32 reg) { return (u8)mesh.MmioRead(reg, 1, 0); }
	void Write(u32 reg, u8 value) { mesh.MmioWrite(reg, value, 1, 0); }

	// Runs a sequence command, waits for its interrupt, returns and
	// clears the interrupt register.
	u8 Sequence(u8 command, u16 count = 0)
	{
		Write(kMeshCountLow, (u8)count);
		Write(kMeshCountHigh, (u8)(count >> 8));
		Write(kMeshSequence, command);
		return WaitInterrupt();
	}

	u8 WaitInterrupt(bool clear = true)
	{
		for (int i = 0; i < 1000 && Read(kMeshInterrupt) == 0; i++)
			clock.Advance(Microseconds(1));
		u8 interrupt = Read(kMeshInterrupt);
		if (clear)
			Write(kMeshInterrupt, 0x07);
		return interrupt;
	}

	// Selects target 0 and sends the CDB through the FIFO after starting
	// the command phase (as the ROM does).
	u8 Command(const std::vector<u8>& cdb)
	{
		Write(kMeshDestinationId, 0);
		Sequence(0x01);						// arbitrate
		Sequence(0x02);						// select
		Write(kMeshCountLow, (u8)cdb.size());
		Write(kMeshCountHigh, 0);
		Write(kMeshSequence, 0x03);
		for (u8 byte : cdb)
			Write(kMeshFifo, byte);
		return WaitInterrupt();
	}

	// Status and message in; returns the status byte.
	u8 Status()
	{
		WaitPhase(kPhaseStatus);
		Sequence(0x04, 1);
		u8 status = Read(kMeshFifo);
		Sequence(0x08, 1);
		Read(kMeshFifo);
		Sequence(0x09);
		return status;
	}

	// Waits for REQ in the phase (a bus free wait first, as the ROM).
	void WaitPhase(u8 phase)
	{
		if ((Read(kMeshBusStatus0) & kBs0Req) == 0)
			Sequence(0x09);
		Expect((Read(kMeshBusStatus0) & (kBs0Req | kPhaseMask))
			== (kBs0Req | phase), "mesh: REQ in the expected phase");
	}
};


static void
MeshTests()
{
	MeshRig rig;

	// a reset of the chip ends with command done (BeOS waits for it)
	rig.Write(kMeshSequence, 0x0e);
	Expect(rig.WaitInterrupt() == kIntDone, "mesh: reset raises done");

	// nobody at ID 5
	rig.Write(kMeshDestinationId, 5);
	rig.Sequence(0x01);
	u8 interrupt = rig.Sequence(0x02);
	Expect((interrupt & kIntException) != 0, "mesh: selection timeout raised");

	// READ(6) block 1, one block, by PIO
	interrupt = rig.Command({ 0x08, 0, 0, 1, 1, 0 });
	Expect(interrupt == kIntDone, "mesh: command phase done");
	u8 bs0 = rig.Read(kMeshBusStatus0);
	Expect((bs0 & kBs0Ack) != 0 && (bs0 & kBs0Req) == 0,
		"mesh: ACK held after the command, no REQ yet");
	Expect((bs0 & kPhaseMask) == kPhaseDataIn,
		"mesh: the target drives data in");

	// the wait for bus free ends with a phase mismatch once REQ comes
	rig.Write(kMeshSequence, 0x09);
	interrupt = rig.WaitInterrupt(false);
	Expect(interrupt == kIntException, "mesh: bus free wait -> exception");
	Expect((rig.Read(kMeshException) & kExcPhaseMismatch) != 0,
		"mesh: ... a phase mismatch");
	rig.Write(kMeshInterrupt, kIntException);
	Expect(rig.Read(kMeshException) == 0,
		"mesh: clearing the exception interrupt clears the exception");
	Expect((rig.Read(kMeshBusStatus0) & kBs0Req) != 0,
		"mesh: REQ for data in after the wait");

	// data in through the FIFO
	rig.Write(kMeshCountLow, 0);
	rig.Write(kMeshCountHigh, 2);
	rig.Write(kMeshSequence, 0x06);
	std::vector<u8> data;
	for (int i = 0; i < 2000 && data.size() < 512; i++) {
		while (rig.Read(kMeshFifoCount) > 0)
			data.push_back(rig.Read(kMeshFifo));
		rig.clock.Advance(Microseconds(1));
	}
	Expect(data.size() == 512, "mesh: 512 bytes through the FIFO");
	Expect(data.size() == 512 && memcmp(data.data(),
		&rig.image->fData[512], 512) == 0, "mesh: the block's data");
	Expect(rig.WaitInterrupt() == kIntDone, "mesh: data in done");

	// status: ACK held after the status byte (drivers wait for REQ to drop
	// before the message in)
	rig.WaitPhase(kPhaseStatus);
	rig.Sequence(0x04, 1);
	bs0 = rig.Read(kMeshBusStatus0);
	Expect((bs0 & kBs0Req) == 0, "mesh: REQ low after the status byte");
	Expect(rig.Read(kMeshFifoCount) == 1 && rig.Read(kMeshFifo) == 0,
		"mesh: status GOOD in the FIFO");
	interrupt = rig.Sequence(0x08, 1);
	Expect(interrupt == kIntDone && rig.Read(kMeshFifo) == 0,
		"mesh: command complete message");
	Expect(rig.Sequence(0x09) == kIntDone, "mesh: bus free");
	Expect((rig.Read(kMeshBusStatus1) & 0x40) == 0, "mesh: BSY released");

	// selection with ATN: the IDENTIFY written after the message out
	// sequence (BeOS), then INQUIRY
	rig.Write(kMeshDestinationId, 0);
	rig.Sequence(0x01);
	Expect(rig.Sequence(0x22) == kIntDone, "mesh: selected with ATN");
	rig.WaitPhase(6);
	rig.Write(kMeshCountLow, 1);
	rig.Write(kMeshCountHigh, 0);
	rig.Write(kMeshSequence, 0x07);
	rig.Write(kMeshFifo, 0x80);
	Expect(rig.WaitInterrupt() == kIntDone && rig.Read(kMeshFifoCount) == 0,
		"mesh: IDENTIFY taken from the FIFO");
	rig.WaitPhase(kPhaseCommand);
	rig.Write(kMeshCountLow, 6);
	rig.Write(kMeshCountHigh, 0);
	rig.Write(kMeshSequence, 0x03);
	for (u8 byte : { 0x12, 0, 0, 0, 36, 0 })
		rig.Write(kMeshFifo, byte);
	rig.WaitInterrupt();
	rig.WaitPhase(kPhaseDataIn);
	rig.Write(kMeshCountLow, 36);
	rig.Write(kMeshCountHigh, 0);
	rig.Write(kMeshSequence, 0x06);
	std::vector<u8> inquiry;
	for (int i = 0; i < 200 && inquiry.size() < 36; i++) {
		while (rig.Read(kMeshFifoCount) > 0)
			inquiry.push_back(rig.Read(kMeshFifo));
		rig.clock.Advance(Microseconds(1));
	}
	Expect(inquiry.size() == 36 && memcmp(&inquiry[8], "QUANTUM", 7) == 0,
		"mesh: INQUIRY after IDENTIFY");
	rig.WaitInterrupt();
	Expect(rig.Status() == 0, "mesh: INQUIRY status GOOD");

	// READ(6) by DMA: INPUT_LAST of 512 bytes to 0x2000, then STOP
	u8* d = &rig.ram[0x1000];
	StoreLE16(d, 512);
	StoreLE16(d + 2, 0x3000);				// INPUT_LAST
	StoreLE32(d + 4, 0x2000);
	StoreLE16(d + 16 + 2, 0x7000);			// STOP
	rig.dma.MmioWrite(0x0c, 0x00100000, 4, 0);	// command pointer (LE)
	rig.dma.MmioWrite(0x00, 0x00800080, 4, 0);	// RUN
	rig.Command({ 0x08, 0, 0, 1, 1, 0 });
	rig.WaitPhase(kPhaseDataIn);
	interrupt = rig.Sequence(0x86, 512);
	Expect(interrupt == kIntDone, "mesh: DMA data in done");
	Expect(memcmp(&rig.ram[0x2000], &rig.image->fData[512], 512) == 0,
		"mesh: DMA data in memory");
	Expect(rig.Status() == 0, "mesh: status after DMA");
}


// #pragma mark - SCSI disk


static void
ScsiDiskTests()
{
	ScsiDisk disk(std::unique_ptr<DiskImage>(new MemoryImage(1 << 20)),
		"QUANTUM", "FIREBALL1080S");
	ScsiCommand command;
	command.cdbLength = 6;
	command.cdb[0] = 0x00;
	disk.Execute(command);		// the unit attention
	command = ScsiCommand();
	command.cdbLength = 6;
	const u8 cdb[6] = { 0x1a, 0, 0x30, 0, 33, 0 };
	memcpy(command.cdb, cdb, sizeof(cdb));
	disk.Execute(command);
	Expect(command.status == 0 && command.dataIn.size() == 33,
		"scsi: mode sense page 0x30 (33 bytes)");
	Expect(command.dataIn.size() == 33 && command.dataIn[12] == 0x30
		&& memcmp(&command.dataIn[14], "APPLE COMPUTER, INC", 19) == 0,
		"scsi: Apple's name in page 0x30");
}


// #pragma mark - Twin Turbo 128


static void
TwinTurboTests()
{
	Clock clock;
	PhysBus bus;
	ImsTwinTurbo128 card(bus, clock.scheduler, kTicksPerSecond, 8 << 20,
		std::vector<u8>(), 0);
	bool line = false;
	card.SetInterruptCallback([&line](bool level) { line = level; });
	// registers as the driver writes them (stwbrx: little endian)
	auto write = [&card](u32 reg, u32 value) {
		card.MmioWrite(ImsTwinTurbo128::kRegisterBase + reg,
			BSwap32(value), 4, 0);
	};
	auto read = [&card](u32 reg) {
		return BSwap32(card.MmioRead(ImsTwinTurbo128::kRegisterBase + reg, 4,
			0));
	};

	write(0x60, 0x0001);				// STGCTL: timing on, VBL off
	clock.Advance(kTicksPerSecond / 30);
	Expect((read(0x90) & 0x10) != 0, "tt128: vertical blank in SSTATUS");
	Expect(!line, "tt128: no interrupt while disabled");
	write(0x60, 0x0401);				// VBL interrupt on
	Expect(line, "tt128: interrupt once enabled");
	write(0x90, read(0x90) | 0x10);		// the driver's acknowledge
	Expect(!line && (read(0x90) & 0x10) == 0,
		"tt128: acknowledged by writing 0x10");
	Expect((read(0x90) & 0xf00) == 0x200, "tt128: chip version kept");
	clock.Advance(kTicksPerSecond / 30);
	Expect(line, "tt128: the next frame interrupts again");
	write(0x60, 0x0001);
	Expect(!line, "tt128: disabling drops the line");

	// the drawing engine: a 16 x 4 fill at (8, 2) of a 640 byte pitch,
	// then a copy of it 100 bytes to the right
	write(0x0c, 2 * 640 + 8);			// DSA
	write(0x10, (3 << 16) | 15);		// CNT: 4 rows of 16 bytes
	write(0x14, 640);					// DP_OCTL
	write(0x18, 0xb4b4b4b4);			// CLR
	write(0x28, 0x00200000);			// BLTCTL: fill
	const std::vector<u8>& vram = card.Vram();
	Expect(vram[2 * 640 + 8] == 0xb4 && vram[5 * 640 + 23] == 0xb4
		&& vram[2 * 640 + 7] == 0 && vram[2 * 640 + 24] == 0
		&& vram[6 * 640 + 8] == 0, "tt128: fill");
	write(0x00, 2 * 640 + 8);			// S1SA
	write(0x08, (640 << 16) | 640);		// SP
	write(0x0c, 2 * 640 + 108);
	write(0x28, 0x05);					// BLTCTL: copy
	Expect(vram[5 * 640 + 123] == 0xb4 && vram[5 * 640 + 124] == 0,
		"tt128: copy");

	// byte order: the engine stores CLR little endian, whatever SCR's byte
	// swap (which is the display's); BeOS swaps 32-bit colors for it
	write(0x0c, 0);
	write(0x10, 3);						// one row of 4 bytes
	write(0x18, 0x33221100);			// CLR: 0x112233, swapped
	write(0x8c, 0x300);
	write(0x28, 0x00200000);
	Expect(vram[0] == 0x00 && vram[1] == 0x11 && vram[2] == 0x22
		&& vram[3] == 0x33, "tt128: a fill stores CLR little endian");

	// and the display: 16 x 1 pixels of 32 bits, the first one the fill
	struct FrameCopy : public VideoSink {
		std::vector<u32> pixels;
		virtual void FrameReady(const u32* data, int width, int height,
			int stride)
		{
			pixels.assign(data, data + (size_t)width * (height > 0));
		}
	} sink;
	card.SetVideoSink(&sink);
	auto dac = [&card](int index, u8 value) {
		card.MmioWrite(ImsTwinTurbo128::kDacBase, index, 1, 0);
		card.MmioWrite(ImsTwinTurbo128::kDacBase + 10 * 4, value, 1, 0);
	};
	dac(0x18, 0x06);					// 32-bit true color, no gamma
	write(0x34, 0);						// HEB
	write(0x38, 1);						// HSB: 16 pixels
	write(0x44, 0);						// VEB
	write(0x48, 1);						// VSB: one line
	write(0x64, 0);						// SSR
	write(0x6c, 16);					// SPR
	write(0x8c, 0x300);
	clock.Advance(kTicksPerSecond / 30);
	Expect(!sink.pixels.empty() && sink.pixels[0] == 0x112233,
		"tt128: a big endian pixel with the 32-bit swap");
	write(0x8c, 0x000);
	write(0x18, 0x00112233);			// the same pixel, little endian
	write(0x28, 0x00200000);
	clock.Advance(kTicksPerSecond / 30);
	Expect(!sink.pixels.empty() && sink.pixels[0] == 0x112233,
		"tt128: a little endian pixel without the swap");
}


// #pragma mark - SWIM3


static void
Swim3Tests()
{
	Clock clock;
	PhysBus bus;
	std::vector<u8> ram(0x10000, 0);
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	DbdmaChannel dma(bus, "floppy");
	Swim3 swim(clock.scheduler, kTicksPerSecond);
	swim.SetDmaChannel(&dma);
	dma.SetDevice(&swim);
	bool line = false;
	swim.SetInterruptCallback([&line](bool level) { line = level; });
	auto write = [&swim](int reg, u8 value) {
		swim.MmioWrite(reg * 16, value, 1, 0);
	};
	auto read = [&swim](int reg) { return (u8)swim.MmioRead(reg * 16, 1, 0); };
	// a sense line: SEL (control bit 0x20) and CA0-2; true when DATA is 0
	auto sense = [&](int which) {
		write(6, 0x20);					// SEL off (written ones clear)
		if ((which & 8) != 0)
			write(7, 0x20);				// SEL on (written ones set)
		write(4, which & 7);
		return (read(7) & 0x08) == 0;
	};
	auto action = [&](int which) {
		sense(which);
		write(4, (which & 7) | 0x08);	// LSTRB
		write(4, which & 7);
	};
	Expect(!sense(7), "swim3: no drive before one is enabled");
	write(7, 0x04);						// drive 2
	Expect(!sense(7), "swim3: no second drive");
	write(6, 0x04);
	write(7, 0x02);						// drive 1
	Expect(sense(7), "swim3: drive present");
	Expect(!sense(8), "swim3: no disk");
	std::unique_ptr<MemoryImage> image(new MemoryImage(1474560));
	// cylinder 2, head 1, sector 3: offset ((2 * 2 + 1) * 18 + 2) * 512
	u32 offset = ((2 * 2 + 1) * 18 + 2) * 512;
	for (int i = 0; i < 512; i++)
		image->fData[offset + i] = (u8)(i ^ 0x5a);
	MemoryImage* disk = image.get();
	std::string error;
	Expect(swim.InsertDisk(std::move(image), false, error), "swim3: insert");
	Expect(sense(8) && sense(15), "swim3: 1.44 MB disk in");
	Expect(!sense(3), "swim3: disk changed is high after the insertion");
	action(12);
	Expect(sense(3), "swim3: action 12 clears it");
	write(7, 0x01);						// interrupts on
	action(2);							// motor on
	Expect(sense(2), "swim3: motor on");
	// seek 2 cylinders up
	action(0);
	write(9, 2);
	write(15, 0x02);					// seek done
	write(7, 0x80);						// DO_SEEK
	clock.Advance(kTicksPerSecond / 50);
	Expect(line && (read(8) & 0x02) != 0, "swim3: seek done");
	Expect(!sense(10), "swim3: off track zero");
	// read sector 3 of head 1 into 0x2000
	u8* d = &ram[0x1000];
	StoreLE16(d, 512);
	StoreLE16(d + 2, 0x3000);			// INPUT_LAST
	StoreLE32(d + 4, 0x2000);
	StoreLE16(d + 16 + 2, 0x7000);		// STOP
	dma.MmioWrite(0x0c, 0x00100000, 4, 0);
	dma.MmioWrite(0x00, 0x00800080, 4, 0);
	sense(12);							// head 1
	write(13, 3);
	write(14, 1);
	write(15, 0x08);					// transfer done
	write(7, 0x08);						// DO_ACTION
	clock.Advance(kTicksPerSecond / 4);	// within a revolution
	Expect((read(8) & 0x08) != 0, "swim3: transfer done");
	bool same = true;
	for (int i = 0; i < 512; i++)
		same &= ram[0x2000 + i] == (u8)(i ^ 0x5a);
	Expect(same, "swim3: the sector by DMA");
	Expect(read(10) == (0x80 | 2) && read(11) == 3,
		"swim3: current track (head 1 in bit 7) and sector");

	// scans: the disk turns on while nobody looks (50 ms: 4.5 sectors)
	write(6, 0x08);
	write(15, 0x04);					// sector seen
	read(8);
	write(7, 0x08);
	clock.Advance(kTicksPerSecond / 80);
	int first = read(8) & 0x04 ? read(11) : -1;
	write(6, 0x08);
	clock.Advance(kTicksPerSecond / 20);
	read(8);
	write(7, 0x08);
	clock.Advance(kTicksPerSecond / 80);
	int second = read(8) & 0x04 ? read(11) : -1;
	write(6, 0x08);
	// 62.5 ms from the first look to the second, give or take the sector
	// each read found under the head
	int moved = (second - first + 18) % 18;
	Expect(first > 0 && second > 0 && moved >= 4 && moved <= 6,
		"swim3: the sectors a scan sees follow the time");

	// the Mac OS ROM reads the sense line in bit 2 as well
	write(6, 0x20);
	write(4, 7);
	Expect((read(7) & 0x0c) == 0, "swim3: drive present in bits 2 and 3");
	write(4, 6);
	Expect((read(7) & 0x0c) == 0x0c, "swim3: double sided in bits 2 and 3");

	// write sector 5 of head 1 as Linux does: preamble, data, postamble
	std::vector<u8> preamble(22, 0x4e);
	preamble.insert(preamble.end(), 12, 0);
	static const u8 kMarks[] = { 0x99, 0xa1, 0x99, 0xa1, 0x99, 0xa1, 0x99,
		0xfb, 0x99, 0x0f };
	preamble.insert(preamble.end(), kMarks, kMarks + sizeof(kMarks));
	std::vector<u8> postamble = { 0x99, 0x04 };
	postamble.insert(postamble.end(), 16, 0x4e);
	memcpy(&ram[0x3000], preamble.data(), preamble.size());
	for (int i = 0; i < 512; i++)
		ram[0x3100 + i] = (u8)(i * 7);
	ram[0x3100] = 0x99;					// data is not escaped
	memcpy(&ram[0x3400], postamble.data(), postamble.size());
	memset(d, 0, 64);
	StoreLE16(d, (u16)preamble.size());
	StoreLE32(d + 4, 0x3000);			// OUTPUT_MORE
	StoreLE16(d + 16, 512);
	StoreLE32(d + 20, 0x3100);
	StoreLE16(d + 32, (u16)postamble.size());
	StoreLE16(d + 34, 0x1000);			// OUTPUT_LAST
	StoreLE32(d + 36, 0x3400);
	StoreLE16(d + 50, 0x7000);
	dma.MmioWrite(0x00, 0x00000080, 4, 0);	// stop
	dma.MmioWrite(0x0c, 0x00100000, 4, 0);
	dma.MmioWrite(0x00, 0x00800080, 4, 0);
	write(6, 0x18);						// the read's action ends
	sense(12);
	write(13, 5);
	write(14, 1);
	read(8);
	write(7, 0x18);						// WRITE_SECTORS, DO_ACTION
	clock.Advance(kTicksPerSecond / 4);	// the sector comes around
	Expect((read(8) & 0x08) != 0, "swim3: write done");
	u32 sector5 = ((2 * 2 + 1) * 18 + 4) * 512;
	same = true;
	for (int i = 1; i < 512; i++)
		same &= disk->fData[sector5 + i] == (u8)(i * 7);
	Expect(same && disk->fData[sector5] == 0x99,
		"swim3: the data between the marks and the CRC");
	Expect(LoadLE16(d + 46) != 0, "swim3: the postamble taken");
	write(6, 0x18);

	// format cylinder 2 head 0: the sectors the IDs name get the fill
	std::vector<u8> track(80, 0x4e);
	for (int sector = 1; sector <= 18; sector++) {
		static const u8 kId[] = { 0x99, 0xa1, 0x99, 0xa1, 0x99, 0xa1, 0x99,
			0xfe };
		track.insert(track.end(), 12, 0);
		track.insert(track.end(), kId, kId + sizeof(kId));
		const u8 chrn[] = { 2, 0, (u8)sector, 2, 0x99, 0x04 };
		track.insert(track.end(), chrn, chrn + sizeof(chrn));
		track.insert(track.end(), 22, 0x4e);
		track.insert(track.end(), 12, 0);
		track.insert(track.end(), kMarks, kMarks + sizeof(kMarks));
		track.insert(track.end(), 512, 0xf6);
		const u8 crc[] = { 0x99, 0x04 };
		track.insert(track.end(), crc, crc + 2);
		track.insert(track.end(), 84, 0x4e);
	}
	memcpy(&ram[0x4000], track.data(), track.size());
	memset(d, 0, 64);
	StoreLE16(d, 0x8000);				// more than a track
	StoreLE16(d + 2, 0x1000);
	StoreLE32(d + 4, 0x4000);
	StoreLE16(d + 18, 0x7000);
	dma.MmioWrite(0x00, 0x00000080, 4, 0);
	dma.MmioWrite(0x0c, 0x00100000, 4, 0);
	dma.MmioWrite(0x00, 0x00800080, 4, 0);
	sense(4);							// head 0
	write(7, 0x40);						// FORMAT
	read(8);
	write(7, 0x08);
	clock.Advance(kTicksPerSecond / 4);
	Expect((read(8) & 0x08) != 0, "swim3: track formatted");
	u32 cylinder2 = (2 * 2 + 0) * 18 * 512;
	same = true;
	for (u32 i = 0; i < 18 * 512; i++)
		same &= disk->fData[cylinder2 + i] == 0xf6;
	Expect(same && disk->fData[sector5 + 1] == 7,
		"swim3: format fills its track only");
	write(6, 0x48);
}


// The 524 bytes of a GCR data field from its 703 nibbles (after the sector
// number), as the Mac OS ROM's .Sony driver decodes them (0xFFCF97EA);
// false if the checksum is wrong.
static bool
RomGcrDecode(const u8* nibbles, u8* bytes)
{
	u8 ca = 0, cb = 0, cc = 0;
	for (int i = 0; i < 175; i++) {
		u8 top = (u8)(*nibbles++ << 2);
		u8 va = (top & 0xc0) | *nibbles++;
		bool carry = (cc & 0x80) != 0;
		cc = (u8)(cc << 1 | cc >> 7);
		va ^= cc;
		u32 sum = ca + va + carry;
		ca = (u8)sum;
		top = (u8)(top << 2 | top >> 6);
		u8 vb = ((top & 0xc0) | *nibbles++) ^ ca;
		sum = cb + vb + (sum >> 8);
		cb = (u8)sum;
		*bytes++ = va;
		*bytes++ = vb;
		if (i == 174)
			break;
		top = (u8)(top << 2 | top >> 6);
		u8 vc = ((top & 0xc0) | *nibbles++) ^ cb;
		cc = (u8)(cc + vc + (sum >> 8));
		*bytes++ = vc;
	}
	u8 top = (u8)(*nibbles++ << 2);
	bool ok = ((top & 0xc0) | nibbles[0]) == ca;
	top = (u8)(top << 2 | top >> 6);
	ok &= ((top & 0xc0) | nibbles[1]) == cb;
	top = (u8)(top << 2 | top >> 6);
	return ok && ((top & 0xc0) | nibbles[2]) == cc;
}


// The other way, for writes.
static void
TestGcrEncode(const u8* bytes, u8* nibbles)
{
	u8 ca = 0, cb = 0, cc = 0;
	for (int i = 0; i < 175; i++) {
		bool last = i == 174;
		u8 va = bytes[3 * i], vb = bytes[3 * i + 1];
		u8 vc = last ? 0 : bytes[3 * i + 2];
		bool carry = (cc & 0x80) != 0;
		cc = (u8)(cc << 1 | cc >> 7);
		u32 sum = ca + va + carry;
		ca = (u8)sum;
		u8 ea = va ^ cc;
		sum = cb + vb + (sum >> 8);
		cb = (u8)sum;
		u8 eb = vb ^ ca;
		u8 ec = 0;
		if (!last) {
			cc = (u8)(cc + vc + (sum >> 8));
			ec = vc ^ cb;
		} else
			ec = cb;
		*nibbles++ = ((ea >> 2) & 0x30) | ((eb >> 4) & 0x0c) | (ec >> 6);
		*nibbles++ = ea & 0x3f;
		*nibbles++ = eb & 0x3f;
		if (!last)
			*nibbles++ = ec & 0x3f;
	}
	*nibbles++ = ((ca >> 2) & 0x30) | ((cb >> 4) & 0x0c) | (cc >> 6);
	*nibbles++ = ca & 0x3f;
	*nibbles++ = cb & 0x3f;
	*nibbles++ = cc & 0x3f;
}


// 800 KB disks: GCR, zones of speed and sectors, interleaved sectors.
static void
Swim3GcrTests()
{
	Clock clock;
	PhysBus bus;
	std::vector<u8> ram(0x10000, 0);
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	DbdmaChannel dma(bus, "floppy");
	Swim3 swim(clock.scheduler, kTicksPerSecond);
	swim.SetDmaChannel(&dma);
	dma.SetDevice(&swim);
	auto write = [&swim](int reg, u8 value) {
		swim.MmioWrite(reg * 16, value, 1, 0);
	};
	auto read = [&swim](int reg) { return (u8)swim.MmioRead(reg * 16, 1, 0); };
	auto sense = [&](int which) {
		write(6, 0x20);
		if ((which & 8) != 0)
			write(7, 0x20);
		write(4, which & 7);
		return (read(7) & 0x08) == 0;
	};
	auto action = [&](int which) {
		sense(which);
		write(4, (which & 7) | 0x08);
		write(4, which & 7);
	};
	auto startDma = [&](u16 command, u16 count, u32 address) {
		u8* d = &ram[0x1000];
		memset(d, 0, 32);
		StoreLE16(d, count);
		StoreLE16(d + 2, command);
		StoreLE32(d + 4, address);
		StoreLE16(d + 18, 0x7000);
		dma.MmioWrite(0x00, 0x00000080, 4, 0);
		dma.MmioWrite(0x0c, 0x00100000, 4, 0);
		dma.MmioWrite(0x00, 0x00800080, 4, 0);
	};
	write(7, 0x02);						// drive 1
	std::unique_ptr<MemoryImage> image(new MemoryImage(819200));
	// cylinder 17 (zone 1: 11 sectors), head 1, sector 7: 16 cylinders of
	// 12 sectors on two sides, one of 11, then head 1's 11
	u32 offset = (16 * 12 * 2 + 11 * 2 + 11 + 7) * 512;
	for (int i = 0; i < 512; i++)
		image->fData[offset + i] = (u8)(i * 3 + 1);
	MemoryImage* disk = image.get();
	std::string error;
	Expect(swim.InsertDisk(std::move(image), false, error),
		"swim3 gcr: 800 KB disk inserted");
	Expect(sense(8) && !sense(15), "swim3 gcr: a disk, not two meg media");
	action(2);							// motor on
	action(13);							// the drive in GCR mode
	Expect(sense(13), "swim3 gcr: drive in GCR mode");
	write(5, 0x4c);						// setup as Mac OS: GCR
	action(0);
	write(9, 17);
	write(7, 0x80);
	clock.Advance(kTicksPerSecond / 10);

	// read sector 7 of head 1: its data field's nibbles as values
	startDma(0x3000, 0x8000, 0x2000);
	sense(12);
	write(13, 7);
	write(14, 1);
	write(15, 0x08);
	read(8);
	write(7, 0x08);
	clock.Advance(kTicksPerSecond / 2);	// a revolution at 429 rpm: 140 ms
	Expect((read(8) & 0x08) != 0, "swim3 gcr: transfer done");
	u8 bytes[524];
	bool decoded = ram[0x2000] == 7 && RomGcrDecode(&ram[0x2001], bytes);
	bool same = decoded;
	for (int i = 0; i < 12; i++)
		same &= bytes[i] == 0;
	for (int i = 0; i < 512; i++)
		same &= bytes[12 + i] == (u8)(i * 3 + 1);
	Expect(decoded, "swim3 gcr: the data field decodes as the ROM does it");
	Expect(same, "swim3 gcr: tags and data of the sector");
	Expect(read(10) == (0x80 | 17) && read(11) == 7 && read(12) == 0x22,
		"swim3 gcr: track (head 1), sector and format 0x22");
	write(6, 0x08);

	// the sectors pass two apart: after 0 comes 6 (11 a track)
	write(15, 0x04);
	int previous = -1;
	bool interleaved = false;
	for (int i = 0; i < 24; i++) {
		read(8);
		write(7, 0x08);
		clock.Advance(kTicksPerSecond * 60 / 429 / 11 + 1);
		int sector = (read(8) & 0x04) != 0 ? read(11) : -1;
		write(6, 0x08);
		interleaved |= previous == 0 && sector == 6;
		previous = sector;
	}
	Expect(interleaved, "swim3 gcr: sector 6 follows sector 0");

	// write sector 3 of head 1: sync bytes, D5 AA AD, the nibbles
	u8 data[524] = {};
	for (int i = 0; i < 512; i++)
		data[12 + i] = (u8)(0xa5 ^ i);
	std::vector<u8> stream(6, 0xff);
	stream.push_back(0xd5);
	stream.push_back(0xaa);
	stream.push_back(0xad);
	stream.push_back(3);
	u8 nibbles[703];
	TestGcrEncode(data, nibbles);
	stream.insert(stream.end(), nibbles, nibbles + 703);
	stream.push_back(0xde);
	stream.push_back(0xaa);
	stream.push_back(0xff);
	memcpy(&ram[0x3000], stream.data(), stream.size());
	startDma(0x1000, (u16)stream.size(), 0x3000);
	write(13, 3);
	write(14, 1);
	read(8);
	write(7, 0x18);						// WRITE, DO_ACTION
	clock.Advance(kTicksPerSecond / 2);
	Expect((read(8) & 0x08) != 0, "swim3 gcr: write done");
	u32 sector3 = (16 * 12 * 2 + 11 * 2 + 11 + 3) * 512;
	same = true;
	for (int i = 0; i < 512; i++)
		same &= disk->fData[sector3 + i] == (u8)(0xa5 ^ i);
	Expect(same, "swim3 gcr: the sector written");
	write(6, 0x18);

	// format cylinder 17 head 1 as Mac OS does: self-sync groups (disk
	// bytes, top bit clear), D5 AA and the values of 96 and AD, the address
	// field's values, a data field of 0xE5 bytes; longer than a revolution
	std::vector<u8> track;
	static const u8 kSync[] = { 0x3f, 0xbf, 0x1e, 0x34, 0x3c, 0x3f };
	for (int i = 0; i < 200; i++)
		track.insert(track.end(), kSync, kSync + sizeof(kSync));
	u8 fill[524];
	memset(fill, 0, 12);
	memset(fill + 12, 0xe5, 512);
	u8 fillNibbles[703];
	TestGcrEncode(fill, fillNibbles);
	for (int i = 0; i < 11; i++) {
		int sector = (i % 2 == 0 ? i / 2 : 6 + i / 2);
		const u8 address[] = { 0xd5, 0xaa, 0x00, 17, (u8)sector, 0x20, 0x22,
			(u8)((17 ^ sector ^ 0x20 ^ 0x22) & 0x3f), 0x27, 0xaa };
		track.insert(track.end(), address, address + sizeof(address));
		track.insert(track.end(), kSync, kSync + sizeof(kSync));
		const u8 mark[] = { 0xd5, 0xaa, 0x0b, (u8)sector };
		track.insert(track.end(), mark, mark + sizeof(mark));
		track.insert(track.end(), fillNibbles, fillNibbles + 703);
		track.push_back(0x27);
		track.push_back(0xaa);
		for (int j = 0; j < 8; j++)
			track.insert(track.end(), kSync, kSync + sizeof(kSync));
	}
	memcpy(&ram[0x4000], track.data(), track.size());
	startDma(0x1000, (u16)track.size(), 0x4000);
	sense(12);
	write(7, 0x40);						// FORMAT
	write(15, 0x08);
	read(8);
	write(7, 0x08);
	// until the transfer is done (the status's interrupt bit)
	for (int ms = 0; ms < 500 && (read(7) & 0x02) == 0; ms++)
		clock.Advance(kTicksPerSecond / 1000);
	Expect((read(8) & 0x08) != 0, "swim3 gcr: track formatted");
	u32 head1 = (16 * 12 * 2 + 11 * 2 + 11) * 512;
	same = true;
	for (u32 i = 0; i < 11 * 512; i++)
		same &= disk->fData[head1 + i] == 0xe5;
	Expect(same, "swim3 gcr: its 11 sectors filled");
	// the format ends just before the index: sector 0 passes first
	write(6, 0x48);
	write(15, 0x04);
	read(8);
	write(7, 0x08);
	clock.Advance(kTicksPerSecond * 60 / 429 / 11);
	Expect((read(8) & 0x04) != 0 && read(11) == 0,
		"swim3 gcr: sector 0 first after the format");
	write(6, 0x08);

	// in MFM mode no sector of a GCR disk passes
	write(5, 0x00);
	read(8);
	write(7, 0x08);
	clock.Advance(kTicksPerSecond / 5);
	Expect((read(8) & 0x04) == 0, "swim3 gcr: none seen in MFM mode");
	write(6, 0x08);
}


static void
FloppyImageTests()
{
	std::string error;
	// cut short (as the zeros at the end were): the HFS volume tells 800 KB
	std::unique_ptr<MemoryImage> raw(new MemoryImage(300 * 512));
	StoreBE16(&raw->fData[1024], 0x4244);
	StoreBE16(&raw->fData[1024 + 18], 1594);
	StoreBE32(&raw->fData[1024 + 20], 512);
	StoreBE16(&raw->fData[1024 + 28], 4);
	StoreBE16(&raw->fData[1024 + 134], 0);
	raw->fData[4 * 512 + 8] = 1;		// the extents B-tree's header
	StoreBE16(&raw->fData[4 * 512 + 10], 3);
	StoreBE16(&raw->fData[4 * 512 + 32], 512);
	std::unique_ptr<FloppyImage> floppy = FloppyImage::Wrap(std::move(raw),
		error);
	Expect(floppy && floppy->GetKind() == FloppyImage::kGcr800
		&& floppy->Size() == 819200, "floppy: short 800 KB HFS image");
	u8 sector[512];
	Expect(floppy && floppy->Read(1599 * 512, sector, 512) && sector[0] == 0,
		"floppy: zeros past the end");

	// compressed Disk Copy 6 without its map: no number of sectors, and
	// nothing to put back together
	floppy = FloppyImage::Wrap(std::unique_ptr<DiskImage>(
		new MemoryImage(300 * 512 + 7)), error);
	Expect(!floppy && error.find("Disk Copy 6") != std::string::npos,
		"floppy: compressed Disk Copy 6 image refused");

	// one that can be: an 800 KB HFS volume with 1100 used blocks from the
	// first, stored as Disk Copy 6 does: the first 4 sectors as they are,
	// chunks of 512 sectors (the first ADC compressed: runs of bytes as they
	// are and copies; the second stored; the third compressed), the spare
	// master directory block at the end
	std::vector<u8> volume(819200, 0);
	StoreBE16(&volume[1024], 0x4244);
	StoreBE16(&volume[1024 + 14], 3);		// the bitmap
	StoreBE16(&volume[1024 + 18], 1594);
	StoreBE32(&volume[1024 + 20], 512);
	StoreBE16(&volume[1024 + 28], 4);
	for (u32 block = 0; block < 1100; block++)
		volume[1536 + block / 8] |= 0x80 >> (block % 8);
	for (u32 i = 2048; i < 2048 + 1100 * 512; i++)
		volume[i] = (u8)((i / 512) * 7 + (i % 512 < 256 ? 0 : i));
	volume[1598 * 512] = 0x42;
	std::vector<u8> ndif(volume.begin(), volume.begin() + 2048);
	auto compress = [&ndif](const u8* data, size_t size) {
		// a run of 128 as it is, then copies of 64 bytes from 128 back
		size_t done = 0;
		while (done < size) {
			size_t literal = std::min<size_t>(128, size - done);
			ndif.push_back((u8)(0x80 + literal - 1));
			ndif.insert(ndif.end(), data + done, data + done + literal);
			done += literal;
			if (done >= 128 && done + 64 <= size
				&& memcmp(data + done, data + done - 128, 64) == 0) {
				ndif.push_back(0x40 + 60);
				ndif.push_back(0);
				ndif.push_back(127);
				done += 64;
			}
		}
	};
	const u8* used = &volume[2048];
	compress(used, 512 * 512);
	ndif.insert(ndif.end(), used + 512 * 512, used + 1024 * 512);
	compress(used + 1024 * 512, 76 * 512);
	ndif.insert(ndif.end(), volume.begin() + 1598 * 512,
		volume.begin() + 1599 * 512);
	std::unique_ptr<MemoryImage> compressed(new MemoryImage(ndif.size()));
	compressed->fData = ndif;
	floppy = FloppyImage::Wrap(std::move(compressed), error);
	std::vector<u8> back(819200);
	Expect(floppy && floppy->GetKind() == FloppyImage::kGcr800
		&& floppy->IsReadOnly() && floppy->Read(0, back.data(), back.size())
		&& back == volume, "floppy: compressed Disk Copy 6 put back together");

	// Disk Copy 4.2: the checksums follow the writes
	std::unique_ptr<MemoryImage> dc42(new MemoryImage(84 + 409600
		+ 800 * 12));
	MemoryImage* file = dc42.get();
	file->fData[0] = 4;
	memcpy(&file->fData[1], "Test", 4);
	StoreBE32(&file->fData[64], 409600);
	StoreBE32(&file->fData[68], 800 * 12);
	file->fData[80] = 0;
	file->fData[81] = 0x02;
	file->fData[82] = 0x01;
	floppy = FloppyImage::Wrap(std::move(dc42), error);
	Expect(floppy && floppy->GetKind() == FloppyImage::kGcr400
		&& floppy->IsDiskCopy() && floppy->Sides() == 1,
		"floppy: Disk Copy 4.2 400 KB image");
	memset(sector, 0, sizeof(sector));
	sector[0] = 0x12;
	sector[1] = 0x34;
	u8 tag[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x56, 0x78 };
	Expect(floppy && floppy->Write(512, sector, 512)
		&& floppy->WriteTag(1, tag) && floppy->Flush(),
		"floppy: written and flushed");
	// each word added, the sum turned right a bit; the tags' from the
	// second sector's
	auto checksum = [&file](u32 offset, u32 length) {
		u32 sum = 0;
		for (u32 i = 0; i < length; i += 2) {
			sum += LoadBE16(&file->fData[offset + i]);
			sum = sum >> 1 | sum << 31;
		}
		return sum;
	};
	Expect(LoadBE32(&file->fData[72]) == checksum(84, 409600)
		&& LoadBE32(&file->fData[76]) == checksum(84 + 409600 + 12,
			800 * 12 - 12)
		&& LoadBE32(&file->fData[76]) != 0
		&& LoadBE16(&file->fData[84 + 512]) == 0x1234,
		"floppy: Disk Copy checksums updated");
}


// #pragma mark - AWACS


struct CaptureSink : public AudioSink {
	std::vector<s16> samples;
	u32 rate = 0;
	virtual void Play(const s16* frames, size_t count, u32 sampleRate)
	{
		samples.insert(samples.end(), frames, frames + count * 2);
		rate = sampleRate;
	}
};


static void
AwacsTests()
{
	Clock clock;
	PhysBus bus;
	std::vector<u8> ram(0x10000, 0);
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	DbdmaChannel out(bus, "audio-out");
	DbdmaChannel in(bus, "audio-in");
	Awacs awacs(clock.scheduler, kTicksPerSecond);
	awacs.SetOutputDma(&out);
	awacs.SetInputDma(&in);
	CaptureSink sink;
	awacs.SetSink(&sink);
	auto write = [&awacs](u32 reg, u32 value) {
		awacs.MmioWrite(reg, BSwap32(value), 4, 0);	// little endian
	};
	write(0x00, 0x0200);				// 22050 Hz
	Expect(awacs.SampleRate() == 22050, "awacs: rate select");
	// 441 frames (20 ms) of a ramp, big endian
	const u32 kFrames = 441;
	for (u32 i = 0; i < kFrames; i++) {
		StoreBE16(&ram[0x2000 + i * 4], (u16)(i * 10));
		StoreBE16(&ram[0x2000 + i * 4 + 2], (u16)(-(s16)(i * 10)));
	}
	u8* d = &ram[0x1000];
	StoreLE16(d, kFrames * 4);
	StoreLE16(d + 2, 0x1000);			// OUTPUT_LAST
	StoreLE32(d + 4, 0x2000);
	StoreLE16(d + 16 + 2, 0x7000);		// STOP
	out.MmioWrite(0x0c, 0x00100000, 4, 0);
	out.MmioWrite(0x00, 0x00800080, 4, 0);
	clock.Advance(kTicksPerSecond / 100);	// 10 ms: half of it
	Expect(awacs.FramesPlayed() >= 200 && awacs.FramesPlayed() <= 240,
		"awacs: plays at the sample rate");
	clock.Advance(kTicksPerSecond / 20);
	Expect(awacs.FramesPlayed() == kFrames, "awacs: all frames played");
	Expect(sink.rate == 22050 && sink.samples.size() == kFrames * 2
		&& sink.samples[2 * 100] == 1000 && sink.samples[2 * 100 + 1] == -1000,
		"awacs: samples big endian, left and right");
	// codec register 4: the speaker 6 steps (9 dB) down
	write(0x10, (4 << 12) | (6 << 6) | 6);
	StoreLE16(d + 12, 0);
	out.MmioWrite(0x00, 0x00000080, 4, 0);		// stop (RUN off)
	out.MmioWrite(0x0c, 0x00100000, 4, 0);
	out.MmioWrite(0x00, 0x00800080, 4, 0);
	clock.Advance(kTicksPerSecond / 10);
	Expect(sink.samples.size() == kFrames * 4
		&& sink.samples[2 * (kFrames + 100)] > 340
		&& sink.samples[2 * (kFrames + 100)] < 370,
		"awacs: speaker attenuation");
	Expect((BSwap32(awacs.MmioRead(0x20, 4, 0)) & 0x00400000) != 0,
		"awacs: codec status valid");
}


// #pragma mark - MACE


struct TestNetwork : public NetworkBackend {
	std::vector<std::vector<u8> > sent;
	std::deque<std::vector<u8> > incoming;

	virtual void Send(const u8* frame, size_t length)
	{
		sent.emplace_back(frame, frame + length);
	}
	virtual void Poll()
	{
		while (!incoming.empty() && fReceiver(incoming.front().data(),
				incoming.front().size()))
			incoming.pop_front();
	}
	void Queue(const u8* destination, u16 type, size_t length, u8 fill)
	{
		std::vector<u8> frame(length, fill);
		memcpy(frame.data(), destination, 6);
		static const u8 kSource[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
		memcpy(frame.data() + 6, kSource, 6);
		frame[12] = (u8)(type >> 8);
		frame[13] = (u8)type;
		incoming.push_back(frame);
	}
};


// The reflected CRC of a frame and its FCS leaves this residue.
static bool
FcsGood(const u8* data, size_t length)
{
	u32 crc = 0xffffffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
	}
	return crc == 0xdebb20e3;
}


static void
MaceTests()
{
	Clock clock;
	PhysBus bus;
	std::vector<u8> ram(0x10000, 0);
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	DbdmaChannel tx(bus, "enet-tx");
	DbdmaChannel rx(bus, "enet-rx");
	Mace mace(clock.scheduler, kTicksPerSecond);
	mace.SetTransmitDma(&tx);
	mace.SetReceiveDma(&rx);
	bool line = false;
	mace.SetInterruptCallback([&line](bool level) { line = level; });
	TestNetwork network;
	mace.SetBackend(&network);
	// DBDMA registers are little endian
	auto dma = [](DbdmaChannel& channel, u32 offset, u32 value) {
		channel.MmioWrite(offset, BSwap32(value), 4, 0);
	};
	auto reg = [&mace](int n) { return (u8)mace.MmioRead(n * 16, 1, 0); };
	auto set = [&mace](int n, u8 value) { mace.MmioWrite(n * 16, value, 1, 0); };
	static const u8 kOurs[6] = { 0x00, 0x05, 0x02, 0x95, 0x00, 0x01 };

	Expect(reg(16) == 0x40 && reg(17) == 0x09, "mace: chip ID 0x0940");
	set(11, 0x01);						// BIUCC: software reset
	Expect((reg(11) & 0x01) == 0, "mace: reset done at once");
	set(18, 0x80 | 0x04);				// IAC: ADDRCHG, PHYADDR
	Expect((reg(18) & 0x80) == 0, "mace: ADDRCHG clears");
	for (int i = 0; i < 6; i++)
		set(21, kOurs[i]);
	set(18, 0x02);						// LOGADDR: the filter
	for (int i = 0; i < 8; i++)
		set(20, 0);
	set(9, 0x00);						// IMR: all interrupts
	set(13, 0x03);						// MACCC: ENXMT, ENRCV
	Expect((reg(10) & 0x40) != 0, "mace: TDTREQ when the transmitter is on");

	// transmit: two OUTPUT_LAST frames, the second waits for XMTFS
	for (int i = 0; i < 42; i++)
		ram[0x2000 + i] = (u8)i;
	for (int i = 0; i < 100; i++)
		ram[0x2100 + i] = (u8)(0x80 + i);
	u8* d = &ram[0x1000];
	StoreLE16(d, 42);
	StoreLE16(d + 2, 0x1000);			// OUTPUT_LAST
	StoreLE32(d + 4, 0x2000);
	StoreLE16(d + 16, 100);
	StoreLE16(d + 18, 0x1000);
	StoreLE32(d + 20, 0x2100);
	StoreLE16(d + 34, 0x7000);			// STOP
	dma(tx, 0x0c, 0x1000);
	dma(tx, 0x00, 0x80008000);
	Expect(network.sent.size() == 1 && network.sent[0].size() == 60
		&& network.sent[0][41] == 41 && network.sent[0][42] == 0,
		"mace: a short frame is padded to 60 bytes");
	Expect(LoadLE16(d + 14) != 0 && LoadLE16(d + 30) == 0,
		"mace: the second frame waits");
	Expect((reg(7) & 0x0f) == 1 && (reg(10) & 0x40) == 0,
		"mace: one frame in the FIFO, no TDTREQ");
	Expect(!line, "mace: no interrupt while on the wire");
	clock.Advance(Microseconds(100));
	Expect(line && (reg(10) & 0x80) != 0, "mace: XMTSV and an interrupt");
	Expect(reg(8) == 0x01 && !line, "mace: XMTINT, read clears");
	Expect(reg(3) == 0x80, "mace: XMTFS valid, no errors");
	Expect(network.sent.size() == 2 && network.sent[1].size() == 100
		&& network.sent[1][0] == 0x80 && LoadLE16(d + 30) != 0,
		"mace: reading XMTFS lets the next frame go");
	clock.Advance(Microseconds(200));
	Expect(reg(3) == 0x80 && reg(3) == 0, "mace: XMTSV clears when read");
	reg(8);

	// receive into a ring of two INPUT_LAST buffers
	d = &ram[0x3000];
	for (int i = 0; i < 2; i++) {
		StoreLE16(d + 16 * i, 0x800);
		StoreLE16(d + 16 * i + 2, 0x3000);	// INPUT_LAST
		StoreLE32(d + 16 * i + 4, 0x4000 + 0x800 * i);
	}
	StoreLE16(d + 34, 0x7000);
	dma(rx, 0x0c, 0x3000);
	dma(rx, 0x00, 0x80008000);
	static const u8 kOther[6] = { 0x00, 0x05, 0x02, 0x95, 0x00, 0x02 };
	static const u8 kBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	network.Queue(kOther, 0x0800, 100, 0x11);
	network.Queue(kOurs, 0x0800, 100, 0x22);
	network.Queue(kBroadcast, 0x0806, 42, 0x33);
	clock.Advance(Microseconds(1500));
	Expect(mace.FramesReceived() == 2, "mace: another station's frame "
		"is filtered out");
	u16 resid = LoadLE16(d + 12);
	Expect((LoadLE16(d + 14) & 0x0400) != 0 && resid == 0x800 - 108,
		"mace: frame, FCS and status in the first buffer");
	Expect(ram[0x4000 + 14] == 0x22 && FcsGood(&ram[0x4000], 104),
		"mace: the data and a good FCS");
	Expect(ram[0x4000 + 104] == 104 && ram[0x4000 + 105] == 0,
		"mace: RCVCNT and RCVSTS");
	Expect(LoadLE16(d + 16 + 12) == 0x800 - 68 && ram[0x4800 + 60 + 4] == 64,
		"mace: a short frame is padded, broadcast received");
	Expect(reg(8) == 0x02, "mace: RCVINT");
	Expect((LoadLE16(d + 14) & 0x40) != 0 && (LoadLE16(d + 30) & 0x40) != 0,
		"mace: S6 marks the end of a frame");

	// the receiver full: frames wait in the backend
	for (int i = 0; i < 10; i++)
		network.Queue(kOurs, 0x0800, 200, (u8)i);
	clock.Advance(Microseconds(1500));
	Expect(network.incoming.size() == 2 && (reg(7) >> 4) == 8,
		"mace: eight frames wait in the chip");
	set(11, 0x01);
	Expect(reg(13) == 0 && (reg(7) >> 4) == 0, "mace: reset clears");
	network.incoming.clear();

	// 802.3: pad and FCS stripped; broadcast off; promiscuous
	for (int i = 0; i < 6; i++)
		set(21, kOurs[i]);
	set(13, 0x01 | 0x04);				// ENRCV, DRCVBC
	dma(rx, 0x00, 0x80000000);
	StoreLE16(d + 14, 0);
	dma(rx, 0x0c, 0x3000);
	dma(rx, 0x00, 0x80008000);
	network.Queue(kBroadcast, 0x0800, 100, 0x44);
	network.Queue(kOurs, 30, 60, 0x55);
	clock.Advance(Microseconds(1500));
	Expect(mace.FramesReceived() == 11 && LoadLE16(d + 12) == 0x800 - 48
		&& ram[0x4000 + 44] == 44, "mace: 802.3 frames stripped, "
		"broadcasts refused");
	set(13, 0x01 | 0x80);				// promiscuous
	network.Queue(kOther, 0x0800, 100, 0x66);
	clock.Advance(Microseconds(1500));
	Expect(mace.FramesReceived() == 12, "mace: promiscuous");

	// internal loopback: a sent frame comes back
	set(13, 0x03);
	set(29, 0x04);						// UTR: internal loopback
	dma(rx, 0x00, 0x80000000);
	StoreLE16(d + 14, 0);
	dma(rx, 0x0c, 0x3000);
	dma(rx, 0x00, 0x80008000);
	memcpy(&ram[0x2000], kOurs, 6);
	d = &ram[0x1000];
	StoreLE16(d + 14, 0);
	StoreLE16(d + 34, 0x7000);
	size_t sentBefore = network.sent.size();
	dma(tx, 0x00, 0x80000000);
	dma(tx, 0x0c, 0x1000);
	dma(tx, 0x00, 0x80008000);
	Expect(network.sent.size() == sentBefore && mace.FramesReceived() == 13
		&& memcmp(&ram[0x4000], kOurs, 6) == 0,
		"mace: internal loopback");

	// Apple's transmit chain: OUTPUT_LAST waiting for S5, then LOAD_QUAD
	// of XMTFS from the chip's registers
	set(29, 0);
	dma(tx, 0x00, 0x80000000);			// the rest of the old chain
	clock.Advance(Microseconds(100));
	reg(3);								// the loopback frame's status
	bus.MapDevice(0xf3011000, 0x1000, &mace);
	dma(tx, 0x18, 0x00200020);			// wait select: S5 set
	StoreLE16(d, 60);
	StoreLE16(d + 2, 0x1002);			// OUTPUT_LAST, wait if not
	StoreLE16(d + 14, 0);
	StoreLE16(d + 16, 1);
	StoreLE16(d + 18, 0x5600);			// LOAD_QUAD, key 6
	StoreLE32(d + 20, 0xf3011030);		// XMTFS
	StoreLE32(d + 24, 0);
	StoreLE16(d + 30, 0);
	StoreLE16(d + 34, 0x7000);
	sentBefore = network.sent.size();
	dma(tx, 0x0c, 0x1000);
	dma(tx, 0x00, 0x80008000);
	Expect(network.sent.size() == sentBefore + 1 && LoadLE16(d + 30) == 0,
		"mace: the channel waits for S5");
	clock.Advance(Microseconds(100));
	Expect(LoadLE16(d + 30) != 0 && ram[0x1000 + 24] == 0x80
		&& reg(3) == 0, "mace: LOAD_QUAD reads XMTFS after the frame");
	mace.SetBackend(nullptr);
}


// #pragma mark - DBDMA


struct CountingDevice : public DbdmaChannel::Device {
	u32 taken = 0;
	virtual u32 DmaOutput(const u8* data, u32 length)
	{
		taken += length;
		return length;
	}
};


static void
DbdmaTests()
{
	PhysBus bus;
	std::vector<u8> ram(0x10000, 0);
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	DbdmaChannel channel(bus, "test");
	CountingDevice device;
	channel.SetDevice(&device);
	auto write = [&channel](u32 offset, u32 value) {
		channel.MmioWrite(offset, BSwap32(value), 4, 0);	// little endian
	};
	auto status = [&channel]() {
		return BSwap32(channel.MmioRead(0x04, 4, 0));
	};
	// two chains, each 16 bytes and a STOP
	for (u32 chain : { 0x1000u, 0x1100u }) {
		u8* d = &ram[chain];
		StoreLE16(d, 16);
		StoreLE16(d + 2, 0x1000);		// OUTPUT_LAST
		StoreLE32(d + 4, 0x2000);
		StoreLE16(d + 18, 0x7000);		// STOP
	}
	write(0x0c, 0x1000);
	write(0x00, 0x80008000);			// RUN
	Expect(device.taken == 16 && (status() & 0x8400) == 0x8000,
		"dbdma: a chain runs to its STOP and idles");
	// a new chain while RUN stays set: the device's kick does not start it
	write(0x0c, 0x1100);
	channel.Kick();
	Expect(device.taken == 16, "dbdma: a kick does not wake a STOP");
	write(0x00, 0x10001000);			// WAKE
	Expect(device.taken == 32, "dbdma: WAKE fetches the command anew");
}


// #pragma mark - Grand Central


static void
GrandCentralTests()
{
	PhysBus bus;
	GrandCentral gc(bus);
	bool line = false;
	gc.SetCpuInterruptCallback([&line](bool level) { line = level; });
	// little endian registers at the start of the (unmapped) window
	auto read = [&gc](u32 offset) {
		return BSwap32(gc.MmioRead(offset, 4, 0));
	};
	auto write = [&gc](u32 offset, u32 value) {
		gc.MmioWrite(offset, BSwap32(value), 4, 0);
	};
	write(0x24, 0x80000000 | 0x08 | 0x40000);	// Mac OS's mode
	gc.SetInterrupt(GrandCentral::kIrqEthernetRxDma, true);
	gc.SetInterrupt(GrandCentral::kIrqEthernetRxDma, false);
	Expect(line, "gc: a DMA pulse interrupts");
	write(0x28, 0x80000000);
	Expect(!line && (read(0x2c) & 0x08) != 0,
		"gc: the DMA interrupt stays in the levels");
	write(0x28, 0x08);
	Expect(read(0x2c) == 0, "gc: clearing its bit ends it");
	gc.SetInterrupt(GrandCentral::kIrqVia, true);
	write(0x28, 0x80000000);
	Expect((read(0x2c) & 0x40000) != 0, "gc: a level source in the levels");
	gc.SetInterrupt(GrandCentral::kIrqVia, false);
	Expect(read(0x2c) == 0, "gc: and gone when it drops");
}


// #pragma mark - ESCC


static void
EsccTests()
{
	Clock clock;
	Escc escc(clock.scheduler, kTicksPerSecond);
	bool line = false;
	escc.SetInterruptCallback([&line](bool level) { line = level; });
	auto write = [&escc](int reg, u8 value) {
		if (reg != 0)
			escc.WriteControl(1, (u8)reg);
		escc.WriteControl(1, value);
	};
	auto read0 = [&escc]() { return escc.ReadControl(1); };

	// LocalTalk on channel B, as AppleTalk sets it up
	write(9, 0x40);						// reset channel B
	write(4, 0x20);						// SDLC, x1 clock
	write(10, 0xe0);					// FM0
	write(7, 0x7e);
	write(12, 0x06);					// 230.4 kbit/s
	write(13, 0x00);
	write(14, 0x01);					// baud rate generator on
	write(3, 0xdd);						// receiver: hunt, address search
	Expect((read0() & 0x10) != 0, "escc: SDLC receiver hunts (no flags)");
	Expect((read0() & 0x40) != 0, "escc: Tx underrun/EOM set by the reset");
	write(15, 0x40);					// interrupt on Tx underrun/EOM
	write(1, 0x01);						// external/status interrupts
	write(9, 0x08);						// master interrupt enable
	write(5, 0x6b);						// transmitter on, CRC
	write(0, 0xc0);						// reset the Tx underrun/EOM latch
	Expect((read0() & 0x40) == 0, "escc: latch reset");
	escc.WriteData(1, 0x01);			// an ENQ: destination, source, type
	escc.WriteData(1, 0x02);
	escc.WriteData(1, 0x81);
	clock.Advance(kTicksPerSecond / 1000);
	Expect((read0() & 0x40) != 0, "escc: frame done, latch set");
	Expect(line, "escc: external/status interrupt for the frame's end");
	escc.WriteControl(0, 0x10);			// channel A's WR0: nothing for B
	Expect(line, "escc: channel A's reset leaves B's interrupt");
	write(0, 0x10);						// reset external/status
	Expect(!line, "escc: reset external/status clears it");
	write(1, 0x00);
	Expect((escc.ReadControl(1) & 0x04) != 0, "escc: transmit buffer empty");
}


// #pragma mark - ADB and key codes


// The host's side of Cuda's handshake, as the ROM and Linux's via-cuda
// drive it: the VIA's port B (TIP 0x20 and BYTEACK 0x10 out, TREQ 0x08
// in, all active low) and shift register.
class CudaHost {
public:
	CudaHost(Clock& clock, Via6522& via) : fClock(clock), fVia(via)
	{
		_Write(2, 0x30);					// DDRB: TIP and BYTEACK out
		_Write(0, 0x30);					// both high: idle
	}

	void Send(const std::vector<u8>& packet)
	{
		_Write(11, _Read(11) | 0x1c);		// shift out
		for (size_t i = 0; i < packet.size(); i++) {
			_Write(10, packet[i]);
			if (i == 0)
				_Write(0, 0x10);			// TIP low
			else
				_Write(0, _Read(0) ^ 0x10);	// BYTEACK toggles
			_WaitShift();
		}
		_Write(0, 0x30);					// done
		_WaitShift();
	}

	bool TreqAsserted() { return (_Read(0) & 0x08) == 0; }

	// The synchronization: TIP high with BYTEACK low, then both high.
	void Sync()
	{
		_Write(0, 0x20);
		_WaitShift();
		_Write(0, 0x30);
		_WaitShift();
	}

	// Cuda's next packet (empty if it sends none within a second).
	std::vector<u8> Receive()
	{
		std::vector<u8> packet;
		for (int i = 0; i < 100000 && !TreqAsserted(); i++)
			fClock.Advance(kTicksPerSecond / 100000);
		if (!TreqAsserted())
			return packet;
		_Write(11, _Read(11) & ~0x10);		// shift in
		_Read(10);
		_Write(0, 0x10);					// TIP low
		for (;;) {
			_WaitShift();
			packet.push_back(_Read(10));
			if (!TreqAsserted() || packet.size() > 64)
				break;
			_Write(0, _Read(0) ^ 0x10);
		}
		_Write(0, 0x30);
		_WaitShift();
		return packet;
	}

private:
	void _Write(int reg, u8 value) { fVia.MmioWrite(reg * 0x200, value, 1, 0); }
	u8 _Read(int reg) { return (u8)fVia.MmioRead(reg * 0x200, 1, 0); }

	void _WaitShift()
	{
		for (int i = 0; i < 1000 && (fVia.Ifr() & Via6522::kIfrShift) == 0;
				i++)
			fClock.Advance(kTicksPerSecond / 100000);
	}

	Clock& fClock;
	Via6522& fVia;
};


static void
CudaTests()
{
	Clock clock;
	Via6522 via(clock.scheduler, kTicksPerSecond);
	Cuda cuda(via, clock.scheduler, kTicksPerSecond);
	CudaHost host(clock, via);
	cuda.SetTime(0x12345670);
	host.Send({ 0x01, 0x03 });				// GET_TIME
	std::vector<u8> reply = host.Receive();
	Expect(reply.size() == 7 && reply[2] == 0x03
		&& LoadBE32(&reply[3]) == 0x12345670, "cuda: GET_TIME");

	// one second packets, as Mac OS asks for them: the time first and
	// every fourth second, ticks between
	host.Send({ 0x01, 0x1b, 0x03 });
	reply = host.Receive();
	Expect(reply == std::vector<u8>({ 0x01, 0x00, 0x1b }),
		"cuda: one second mode 3 set");
	std::vector<std::vector<u8> > packets;
	for (int i = 0; i < 5; i++)
		packets.push_back(host.Receive());
	auto timePacket = [](u32 time) {
		return std::vector<u8>({ 0x01, 0x00, 0x03, (u8)(time >> 24),
			(u8)(time >> 16), (u8)(time >> 8), (u8)time });
	};
	Expect(packets[0] == timePacket(0x12345671),
		"cuda: the first one tells the time");
	Expect(packets[1] == std::vector<u8>({ 0x03 })
		&& packets[2] == std::vector<u8>({ 0x03 })
		&& packets[3] == std::vector<u8>({ 0x03 }), "cuda: then ticks");
	Expect(packets[4] == timePacket(0x12345675),
		"cuda: the time again after a multiple of four");
	host.Send({ 0x01, 0x1b, 0x00 });
	host.Receive();
	Expect(host.Receive().empty(), "cuda: mode 0 sends none");

	// a system taking over syncs: Cuda's own packets stop (BeOS started
	// from Mac OS, which asked for them)
	host.Send({ 0x01, 0x1b, 0x03 });
	host.Receive();
	Expect(!host.Receive().empty(), "cuda: one second packets again");
	host.Sync();
	Expect(host.Receive().empty() && host.Receive().empty(),
		"cuda: none after a synchronization");
}


static void
AdbTests()
{
	AdbBus adb;
	std::vector<u8> reply;
	adb.Keyboard().KeyDown(0x08);		// C held at power on
	adb.Reset();
	Expect(adb.Command(0x2c, nullptr, 0, reply) && reply.size() == 2
		&& reply[0] == 0x08, "adb: held key reported after a reset");
	reply.clear();
	Expect(!adb.Command(0x2c, nullptr, 0, reply),
		"adb: nothing more to report");
	adb.Command(0x21, nullptr, 0, reply);		// flush
	reply.clear();
	Expect(adb.Command(0x2c, nullptr, 0, reply) && reply[0] == 0x08,
		"adb: held key reported after a flush");
	adb.Keyboard().KeyUp(0x08);
	reply.clear();
	Expect(adb.Command(0x2c, nullptr, 0, reply) && reply[0] == 0x88,
		"adb: release reported");
	adb.Command(0x21, nullptr, 0, reply);
	reply.clear();
	Expect(!adb.Command(0x2c, nullptr, 0, reply),
		"adb: a released key is not reported again");
}


static void
KeyCodeTests()
{
	bool shifted;
	Expect(KeyNameToAdb("Control") == 0x36, "keys: Control is 0x36");
	Expect(KeyNameToAdb("Left") == 0x3b, "keys: Left is 0x3B");
	Expect(KeyNameToAdb("Command") == 0x37, "keys: Command is 0x37");
	Expect(AsciiToAdb('a', shifted) == 0 && !shifted, "keys: a is 0");
	Expect(AsciiToAdb('C', shifted) == 0x08 && shifted, "keys: C shifted");
	Expect(AsciiToAdb('\n', shifted) == 0x24, "keys: newline is Return");
	Expect(AsciiToAdb('?', shifted) == 0x2c && shifted, "keys: ? shifted /");
	Expect(HaikuKeyToAdb(0x5d) == 0x37, "keys: Haiku left Alt is Command");
	Expect(HaikuKeyToAdb(0) == -1, "keys: no Haiku key 0");

	u32 macTime;
	Expect(MacTimeFromDate(1970, 1, 1, 0, 0, macTime)
		&& macTime == 2082844800u, "mac time: 1970-01-01");
	Expect(MacTimeFromDate(2000, 3, 1, 12, 30, macTime)
		&& macTime == 3034758600u, "mac time: 2000-03-01 12:30");
	Expect(!MacTimeFromDate(2041, 1, 1, 0, 0, macTime),
		"mac time: past the 32-bit clock");
}


int
main(int argc, char** argv)
{
	Log::SetAllLevels(kLogError);
	MeshTests();
	ScsiDiskTests();
	TwinTurboTests();
	Swim3Tests();
	Swim3GcrTests();
	FloppyImageTests();
	AwacsTests();
	MaceTests();
	DbdmaTests();
	GrandCentralTests();
	EsccTests();
	AdbTests();
	CudaTests();
	KeyCodeTests();
	printf("%d checks, %d failures\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
