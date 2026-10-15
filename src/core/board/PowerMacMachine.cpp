/*
 * PowerMac emulator - the Power Macintosh 9500: processors, buses and
 * devices.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "PowerMacMachine.h"

#include "Hammerhead.h"
#include "SmpRunner.h"
#include "../Log.h"
#include "../bus/Bandit.h"
#include "../bus/PhysBus.h"
#include "../cpu/PpcCpu.h"
#include "../macio/Awacs.h"
#include "../macio/Cuda.h"
#include "../macio/Escc.h"
#include "../macio/GrandCentral.h"
#include "../macio/Mesh.h"
#include "../macio/Mace.h"
#include "../macio/Swim3.h"
#include "../macio/Ncr53c94.h"
#include "../pci/ScsiDevice.h"
#include "../storage/CdImage.h"
#include "../storage/DiskImage.h"
#include "../macio/Via6522.h"
#include "../video/ImsTwinTurbo128.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace powermac {

namespace {

const u32 kBandit1Base = 0xf2000000;
const u32 kBandit2Base = 0xf4000000;
const int kGrandCentralDevice = 16;
// PCI slots A1-C1 on the first Bandit, D2-F2 on the second
const int kSlotA1Device = 13;
// Seconds from 1904 (the Macintosh epoch) to 1970.
const s64 kMacEpochOffset = 2082844800;


bool
ReadFile(const std::string& path, std::vector<u8>& data)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	data.clear();
	u8 buffer[65536];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
		data.insert(data.end(), buffer, buffer + count);
	fclose(file);
	return true;
}


bool
WriteFile(const std::string& path, const void* data, size_t size)
{
	std::string temporary = path + ".new";
	FILE* file = fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		return false;
	bool ok = fwrite(data, 1, size, file) == size;
	ok = fclose(file) == 0 && ok;
	if (ok)
		ok = rename(temporary.c_str(), path.c_str()) == 0;
	return ok;
}


// Writes to the ROM (the ROM's memory sizing drives the bus with them)
// go nowhere, quietly.
class RomWrites : public MmioDevice {
public:
	virtual u32 MmioRead(u32 addr, int size, int initiator)
		{ return 0xffffffff; }
	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		PM_LOG(kLogBus, kLogTrace, "ROM write%d %08x = %x ignored",
			size * 8, addr, value);
	}
	virtual const char* MmioName() const { return "rom"; }
};

RomWrites sRomWrites;

}	// namespace


PowerMacMachine::PowerMacMachine(const MachineConfig& config)
	:
	fConfig(config),
	fModel(MachineConfig::Info(config.model)),
	fSaveAll(false),
	fNow(0),
	fPoweredOn(false),
	fDiskBusy(false),
	fClockOffset(0),
	fKeyInterval(0),
	fPowerOnRequested(false)
{
	fTicksPerSecond = (u64)fModel.busHz * kTicksPerBusCycle;
	fQuantum = (u64)config.quantumUs * fTicksPerSecond / 1000000;
	if (fQuantum == 0)
		fQuantum = 1;
}


PowerMacMachine::~PowerMacMachine()
{
	fSmp.reset();
	fNanokernelTimer.reset();
	fKeyTimer.reset();
	fHoldTimer.reset();
	// CPUs unregister from the bus; devices before the bus.
	fCpus.clear();
	fVideo.reset();
	fMesh.reset();
	fScsiExternal.reset();
	fEscc.reset();
	fAwacs.reset();
	fSwim3.reset();
	fMace.reset();
	fCuda.reset();
	fVia.reset();
	fGrandCentral.reset();
	fBandit2.reset();
	fBandit1.reset();
	fHammerhead.reset();
}


bool
PowerMacMachine::Init(std::string& error)
{
	fBus.reset(new PhysBus);

	int cpuCount = fConfig.cpuCount > 0
		? std::min(fConfig.cpuCount, fModel.cpus) : fModel.cpus;
	std::vector<u32> banks = Hammerhead::BankSizesFor(fConfig.ramMB);
	u64 ramSize = 0;
	for (u32 size : banks)
		ramSize += size;
	fConfig.ramMB = (u32)(ramSize >> 20);
	fRam.reset(new u8[ramSize]());
	fHammerhead.reset(new Hammerhead(*fBus, cpuCount == 2));
	fHammerhead->SetMemory(fRam.get(), banks);

	if (!_LoadRom(error))
		return false;

	fBandit1.reset(new Bandit(*fBus, kBandit1Base, 1));
	fBandit2.reset(new Bandit(*fBus, kBandit2Base, 2));
	fGrandCentral.reset(new GrandCentral(*fBus));
	fBandit1->Pci().Attach(kGrandCentralDevice, fGrandCentral.get());
	fGrandCentral->SetEthernetAddress(fConfig.ethernetMac);

	fVia.reset(new Via6522(fScheduler, fTicksPerSecond));
	fGrandCentral->AttachDevice(0x16000, 0x2000, fVia.get());
	fVia->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqVia, level);
	});
	fCuda.reset(new Cuda(*fVia, fScheduler, fTicksPerSecond));
	fCuda->SetPowerCallbacks([this]() { _PowerOff(); },
		[this]() { _RequestPowerOn(); });

	fAwacs.reset(new Awacs(fScheduler, fTicksPerSecond));
	fGrandCentral->AttachDevice(0x14000, 0x1000, fAwacs.get());
	fAwacs->SetOutputDma(&fGrandCentral->Dma(GrandCentral::kDmaAudioOut));
	fAwacs->SetInputDma(&fGrandCentral->Dma(GrandCentral::kDmaAudioIn));
	fAwacs->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqAudio, level);
	});

	fMace.reset(new Mace(fScheduler, fTicksPerSecond));
	fGrandCentral->AttachDevice(0x11000, 0x1000, fMace.get());
	fMace->SetTransmitDma(&fGrandCentral->Dma(GrandCentral::kDmaEthernetTx));
	fMace->SetReceiveDma(&fGrandCentral->Dma(GrandCentral::kDmaEthernetRx));
	fMace->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqEthernet, level);
	});

	fSwim3.reset(new Swim3(fScheduler, fTicksPerSecond));
	fGrandCentral->AttachDevice(0x15000, 0x1000, fSwim3.get());
	fSwim3->SetDmaChannel(&fGrandCentral->Dma(GrandCentral::kDmaFloppy));
	fGrandCentral->Dma(GrandCentral::kDmaFloppy).SetDevice(fSwim3.get());
	fSwim3->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqFloppy, level);
	});
	if (!fConfig.floppyImage.empty()) {
		std::unique_ptr<DiskImage> image = DiskImage::Open(
			fConfig.floppyImage, fConfig.floppyWriteProtected, error);
		if (!image || !fSwim3->InsertDisk(std::move(image),
				fConfig.floppyWriteProtected, error)) {
			error = fConfig.floppyImage + ": " + error;
			return false;
		}
	}

	fEscc.reset(new Escc(fScheduler, fTicksPerSecond));
	fGrandCentral->AttachDevice(0x12000, 0x2000, fEscc.get());
	fEscc->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqSccA, level);
		fGrandCentral->SetInterrupt(GrandCentral::kIrqSccB, level);
	});

	if (!_SetupVideo(error))
		return false;
	if (!_SetupScsi(error))
		return false;

	_LoadPersistentState();

	// Processors: both start at the reset vector; the ROM parks the
	// second one (Hammerhead tells it who it is).
	u32 ticksPerCycle = (u32)((u64)kTicksPerBusCycle * fModel.busHz
		/ fModel.cpuHz);
	for (int i = 0; i < cpuCount; i++) {
		PpcCpu::Config cpuConfig;
		cpuConfig.model = fModel.is604e ? PpcCpu::kPpc604e : PpcCpu::kPpc604;
		cpuConfig.pvr = fConfig.pvr != 0 ? fConfig.pvr : fModel.pvr;
		cpuConfig.hid1 = fModel.hid1;
		cpuConfig.index = i;
		cpuConfig.ticksPerInstruction = std::max<u32>(1,
			ticksPerCycle * fConfig.cpiPercent / 100);
		cpuConfig.ticksPerTimebase = 4 * kTicksPerBusCycle;
		fCpus.emplace_back(new PpcCpu(*fBus, cpuConfig));
	}
	fGrandCentral->SetCpuInterruptCallback([this](bool level) {
		fCpus[0]->SetIntLine(level);
	});
	if (fCpus.size() > 1) {
		fHammerhead->SetSecondaryInterrupt([this](bool level) {
			fCpus[1]->SetIntLine(level);
		});
		fCpus[1]->SetExternalInterruptHook([this]() {
			fHammerhead->SecondaryInterruptTaken();
		});
		fGrandCentral->SetPrimaryIpiEnabled(true);
		fNanokernelTimer.reset(new Timer(fScheduler,
			[this]() { _WatchForNanokernel(); }, "nanokernel-watch"));
	}
	PM_LOG(kLogMachine, kLogInfo, "Power Macintosh %s: %d x %s at %u MHz, "
		"bus %u MHz, %u MB", fModel.name, cpuCount,
		fModel.is604e ? "604e" : "604", fModel.cpuHz / 1000000,
		fModel.busHz / 1000000, fConfig.ramMB);

	fKeyTimer.reset(new Timer(fScheduler, [this]() { _NextQueuedKey(); },
		"key-queue"));
	fHoldTimer.reset(new Timer(fScheduler, [this]() {
		for (auto key = fHeldKeys.rbegin(); key != fHeldKeys.rend(); key++)
			KeyUp(*key);
		fHeldKeys.clear();
	}, "held-keys"));

	PowerOn();
	if (fConfig.threadPerCpu && fCpus.size() == 2)
		fSmp.reset(new SmpRunner(*fBus, fScheduler, *fCpus[0], *fCpus[1]));
	return true;
}


bool
PowerMacMachine::_LoadRom(std::string& error)
{
	std::vector<u8> data;
	if (!ReadFile(fConfig.romPath, data) || data.size() != kRomSize) {
		error = "cannot read the 4 MB ROM: " + fConfig.romPath;
		return false;
	}
	if (memcmp(&data[0x30d064], "Boot TNT", 8) != 0) {
		PM_LOG(kLogMachine, kLogWarning, "%s is not a 7200/7500/8500/9500 "
			"(\"TNT\") ROM", fConfig.romPath.c_str());
	}
	fRom.reset(new u8[kRomSize]);
	memcpy(fRom.get(), data.data(), kRomSize);
	fBus->MapMemory(kRomBase, kRomSize, fRom.get(), false, &sRomWrites);
	return true;
}


// The display card in slot A1, when there is a ROM for it.
bool
PowerMacMachine::_SetupVideo(std::string& error)
{
	if (fConfig.cardRomPath.empty())
		return true;
	std::vector<u8> rom;
	if (!ReadFile(fConfig.cardRomPath, rom) || rom.size() < 0x1000
		|| rom[0] != 0x55 || rom[1] != 0xaa) {
		error = "cannot read the display card's ROM: " + fConfig.cardRomPath;
		return false;
	}
	u32 vram = std::max<u32>(2, std::min<u32>(8, fConfig.vramMB)) << 20;
	fVideo.reset(new ImsTwinTurbo128(*fBus, fScheduler, fTicksPerSecond,
		vram, rom, fConfig.monitor));
	fBandit1->Pci().Attach(kSlotA1Device, fVideo.get());
	fVideo->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqSlotA1, level);
	});
	return true;
}


// The internal bus (MESH: the hard disks) and the external one (53C94:
// also the CD-ROM drive).
bool
PowerMacMachine::_SetupScsi(std::string& error)
{
	fMesh.reset(new Mesh(fScheduler, fTicksPerSecond));
	fGrandCentral->AttachDevice(0x18000, 0x1000, fMesh.get());
	fMesh->SetDmaChannel(&fGrandCentral->Dma(GrandCentral::kDmaScsi1));
	fGrandCentral->Dma(GrandCentral::kDmaScsi1).SetDevice(fMesh.get());
	fMesh->SetInterruptCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqScsi1, level);
	});

	fScsiExternal.reset(new Ncr53c94(fScheduler, fTicksPerSecond,
		25000000));
	fGrandCentral->AttachDevice(0x10000, 0x1000, fScsiExternal.get());
	fScsiExternal->SetDmaChannel(&fGrandCentral->Dma(GrandCentral::kDmaScsi0));
	fGrandCentral->Dma(GrandCentral::kDmaScsi0).SetDevice(
		fScsiExternal.get());
	fScsiExternal->SetIrqCallback([this](bool level) {
		fGrandCentral->SetInterrupt(GrandCentral::kIrqScsi0, level);
	});

	for (int bus = 0; bus < 2; bus++) {
		const MachineConfig::ScsiTarget* targets = bus == 0
			? fConfig.scsiInternal : fConfig.scsiExternal;
		for (int id = 0; id < 7; id++) {
			const MachineConfig::ScsiTarget& target = targets[id];
			std::unique_ptr<ScsiDevice> device;
			if (target.kind == MachineConfig::ScsiTarget::kDisk
				|| target.kind == MachineConfig::ScsiTarget::kSharedBe
				|| target.kind == MachineConfig::ScsiTarget::kSharedMac) {
				std::unique_ptr<DiskImage> image = DiskImage::Open(
					target.image, target.readOnly, error);
				if (!image)
					return false;
				device.reset(new ScsiDisk(std::move(image), "QUANTUM",
					"FIREBALL1080S"));
			} else if (target.kind == MachineConfig::ScsiTarget::kCdRom) {
				// a drive Apple's CD-ROM driver knows (it ignores others)
				std::unique_ptr<ScsiCd> cd(new ScsiCd("SONY",
					"CD-ROM CDU-8003"));
				if (!target.image.empty()) {
					std::unique_ptr<CdImage> image = CdImage::Open(
						target.image, error);
					if (!image)
						return false;
					cd->Insert(std::move(image));
				}
				device = std::move(cd);
			} else
				continue;
			device->SetActivityCallback([this](bool) { fDiskBusy = true; });
			if (bus == 0)
				fMesh->AttachDevice(id, std::move(device));
			else
				fScsiExternal->AttachDevice(id, std::move(device));
		}
	}
	return true;
}


DisplayDevice*
PowerMacMachine::Display() const
{
	return fVideo.get();
}


u32
PowerMacMachine::_HostMacTime()
{
	time_t now = time(nullptr);
	struct tm local;
	localtime_r(&now, &local);
	return (u32)((s64)now + local.tm_gmtoff + kMacEpochOffset);
}


void
PowerMacMachine::_LoadPersistentState()
{
	std::vector<u8> data;
	if (!fConfig.nvramPath.empty() && ReadFile(fConfig.nvramPath, data)
		&& data.size() == GrandCentral::kNvramSize)
		memcpy(fGrandCentral->Nvram(), data.data(), data.size());
	if (!fConfig.pramPath.empty() && ReadFile(fConfig.pramPath, data)
		&& data.size() >= (size_t)Cuda::kPramSize + 8) {
		memcpy(fCuda->Pram(), data.data(), Cuda::kPramSize);
		memcpy(&fClockOffset, &data[Cuda::kPramSize], 8);
	}
}


u8*
PowerMacMachine::Nvram()
{
	return fGrandCentral->Nvram();
}


bool
PowerMacMachine::SavePersistentState(std::string& error)
{
	bool ok = true;
	if (!fConfig.nvramPath.empty()
		&& (fGrandCentral->NvramDirty() || fSaveAll)) {
		if (WriteFile(fConfig.nvramPath, fGrandCentral->Nvram(),
				GrandCentral::kNvramSize))
			fGrandCentral->ClearNvramDirty();
		else {
			error = "cannot write " + fConfig.nvramPath;
			ok = false;
		}
	}
	if (!fConfig.pramPath.empty() && (fCuda->IsDirty() || fSaveAll)) {
		// the clock as the offset to the host's (unless it is fixed)
		if (fConfig.fixedTime == 0)
			fClockOffset = (s64)fCuda->Time() - (s64)_HostMacTime();
		std::vector<u8> data(Cuda::kPramSize + 8);
		memcpy(data.data(), fCuda->Pram(), Cuda::kPramSize);
		memcpy(&data[Cuda::kPramSize], &fClockOffset, 8);
		if (WriteFile(fConfig.pramPath, data.data(), data.size()))
			fCuda->ClearDirty();
		else {
			error = "cannot write " + fConfig.pramPath;
			ok = false;
		}
	}
	if (ok)
		fSaveAll = false;
	return ok;
}


void
PowerMacMachine::SetPersistentPaths(const std::string& nvram,
	const std::string& pram)
{
	fConfig.nvramPath = nvram;
	fConfig.pramPath = pram;
	fSaveAll = true;
}


void
PowerMacMachine::PowerOn()
{
	PM_LOG(kLogMachine, kLogInfo, "power on reset");
	fScheduler.SetNow(fNow);
	memset(fRam.get(), 0, (size_t)fConfig.ramMB << 20);
	fHammerhead->Reset();
	fBandit1->Reset();
	fBandit2->Reset();
	fGrandCentral->Reset();
	fVia->Reset();
	fCuda->Reset();
	fCuda->SetTime(fConfig.fixedTime != 0 ? fConfig.fixedTime
		: (u32)((s64)_HostMacTime() + fClockOffset));
	fEscc->Reset();
	fAwacs->Reset();
	fSwim3->Reset();
	fMace->Reset();
	if (fVideo)
		fVideo->Reset();
	fMesh->Reset();
	fScsiExternal->Reset();
	for (auto& cpu : fCpus) {
		cpu->SetNow(fNow);
		cpu->SetHresetLine(false);
		cpu->HardReset();
	}
	if (fNanokernelTimer)
		fNanokernelTimer->ArmIn(fTicksPerSecond / 10000);
	fPoweredOn = true;
}


// Every 100 us until CPU 0 enters the nanokernel (the ROM's jump at the
// end of its startup, after Open Firmware): then the dual processor card
// shows (Hammerhead.h).
void
PowerMacMachine::_WatchForNanokernel()
{
	u32 pc = fCpus[0]->pc;
	if (pc >= kRomBase + 0x310000 && pc < kRomBase + 0x320000) {
		PM_LOG(kLogMachine, kLogInfo, "nanokernel entered: the second "
			"processor shows");
		fHammerhead->RevealDualProcessor();
		return;
	}
	fNanokernelTimer->ArmIn(fTicksPerSecond / 10000);
}


void
PowerMacMachine::_PowerOff()
{
	PM_LOG(kLogMachine, kLogInfo, "power off");
	fPoweredOn = false;
	for (auto& cpu : fCpus)
		cpu->SetHresetLine(true);
}


void
PowerMacMachine::_RequestPowerOn()
{
	// A device asked (a CPU's I/O access): with the CPUs on their own
	// threads, the reset waits for the end of the time slice.
	if (fSmp)
		fPowerOnRequested = true;
	else
		PowerOn();
}


void
PowerMacMachine::PressInterruptSwitch()
{
	PM_LOG(kLogMachine, kLogInfo, "interrupt switch pressed");
	fGrandCentral->SetInterrupt(GrandCentral::kIrqNmi, true);
	fGrandCentral->SetInterrupt(GrandCentral::kIrqNmi, false);
}


void
PowerMacMachine::KeyDown(u8 adbCode)
{
	fCuda->Adb().Keyboard().KeyDown(adbCode);
	fCuda->InputArrived();
}


void
PowerMacMachine::KeyUp(u8 adbCode)
{
	fCuda->Adb().Keyboard().KeyUp(adbCode);
	fCuda->InputArrived();
}


void
PowerMacMachine::QueueKeys(const std::vector<std::pair<u8, bool> >& events,
	u64 interval)
{
	fKeyQueue.insert(fKeyQueue.end(), events.begin(), events.end());
	fKeyInterval = std::max<u64>(interval, 1);
	if (fKeyTimer && !fKeyTimer->IsArmed())
		_NextQueuedKey();
}


void
PowerMacMachine::HoldKeys(const std::vector<u8>& codes, u64 ticks)
{
	for (u8 code : codes) {
		KeyDown(code);
		fHeldKeys.push_back(code);
	}
	fHoldTimer->ArmIn(std::max<u64>(ticks, 1));
}


void
PowerMacMachine::_NextQueuedKey()
{
	if (fKeyQueue.empty())
		return;
	std::pair<u8, bool> event = fKeyQueue.front();
	fKeyQueue.pop_front();
	if (event.second)
		KeyDown(event.first);
	else
		KeyUp(event.first);
	if (!fKeyQueue.empty())
		fKeyTimer->ArmIn(fKeyInterval);
}


bool
PowerMacMachine::PointerPosition(int& x, int& y)
{
	if (fVideo && fVideo->HardwareCursor(x, y))
		return true;
	// Mac OS draws its pointer: Mouse (0x830) is the point (v, h)
	int v = (s16)fBus->Read16(0x830, kInitiatorDma);
	int h = (s16)fBus->Read16(0x832, kInitiatorDma);
	if (v < 0 || h < 0 || v > 4096 || h > 4096)
		return false;
	x = h;
	y = v;
	return true;
}


ScsiCd*
PowerMacMachine::_Cd(int bus, int id) const
{
	if (id < 0 || id > 6)
		return nullptr;
	ScsiDevice* device = nullptr;
	if (bus == 0 && fMesh)
		device = fMesh->Device(id);
	else if (bus == 1 && fScsiExternal)
		device = fScsiExternal->Device(id);
	return dynamic_cast<ScsiCd*>(device);
}


bool
PowerMacMachine::InsertCd(int bus, int id, const std::string& path,
	std::string& error)
{
	ScsiCd* cd = _Cd(bus, id);
	if (cd == nullptr) {
		error = "no CD-ROM drive at that SCSI ID";
		return false;
	}
	std::unique_ptr<CdImage> image = CdImage::Open(path, error);
	if (!image)
		return false;
	cd->Insert(std::move(image));
	return true;
}


void
PowerMacMachine::EjectCd(int bus, int id)
{
	if (ScsiCd* cd = _Cd(bus, id))
		cd->Eject();
}


std::string
PowerMacMachine::CdPath(int bus, int id) const
{
	ScsiCd* cd = _Cd(bus, id);
	return cd != nullptr ? cd->ImagePath() : std::string();
}


bool
PowerMacMachine::InsertFloppy(const std::string& path, bool writeProtected,
	std::string& error)
{
	std::unique_ptr<DiskImage> image = DiskImage::Open(path, writeProtected,
		error);
	if (!image)
		return false;
	return fSwim3->InsertDisk(std::move(image), writeProtected, error);
}


void
PowerMacMachine::EjectFloppy()
{
	fSwim3->EjectDisk();
}


std::string
PowerMacMachine::FloppyPath() const
{
	return fSwim3->ImagePath();
}


void
PowerMacMachine::MouseMoved(int dx, int dy, u32 buttons)
{
	fCuda->Adb().Mouse().Moved(dx, dy, (buttons & 1) != 0);
	fCuda->InputArrived();
}


void
PowerMacMachine::SetSerialBackend(int port, SerialBackend* backend)
{
	fEscc->SetBackend(port, backend);
}


void
PowerMacMachine::SetNetworkBackend(NetworkBackend* backend)
{
	if (fMace)
		fMace->SetBackend(backend);
}


void
PowerMacMachine::SetVideoSink(VideoSink* sink)
{
	if (fVideo)
		fVideo->SetVideoSink(sink);
}


void
PowerMacMachine::SetAudioSink(AudioSink* sink)
{
	if (fAwacs)
		fAwacs->SetSink(sink);
}


void
PowerMacMachine::RunFor(u64 ticks)
{
	u64 target = fNow + ticks;
	while (fNow < target) {
		u64 sliceEnd = std::min(target, fNow + fQuantum);
		u64 next = fScheduler.NextEventTime();
		if (next < sliceEnd)
			sliceEnd = std::max(next, fNow + 1);
		for (auto& cpu : fCpus) {
			if (cpu->Now() < fNow)
				cpu->SetNow(fNow);
		}
		// A CPU in reset or dozing does next to nothing: then the other
		// one runs on this thread too, without waiting for the worker.
		if (fSmp && !fCpus[0]->IsHalted() && !fCpus[1]->IsHalted())
			fSmp->RunSlice(sliceEnd);
		else {
			for (auto& cpu : fCpus) {
				fScheduler.SetClock(cpu->NowPointer());
				cpu->Run(sliceEnd);
			}
			fScheduler.SetClock(nullptr);
		}
		fNow = sliceEnd;
		fScheduler.SetNow(fNow);
		fScheduler.RunDue(fNow);
		if (fPowerOnRequested.exchange(false))
			PowerOn();
	}
}


u64
PowerMacMachine::InstructionCount() const
{
	u64 count = 0;
	for (const auto& cpu : fCpus)
		count += cpu->InstructionCount();
	return count;
}


bool
PowerMacMachine::CpuActive(int index) const
{
	return index < (int)fCpus.size() && !fCpus[index]->IsHalted();
}


u64
PowerMacMachine::UnclaimedIoCount() const
{
	return fBandit1->Io().UnclaimedCount() + fBandit2->Io().UnclaimedCount();
}


std::string
PowerMacMachine::CpuState(int index) const
{
	const PpcCpu* cpu = fCpus[index].get();
	char line[160];
	snprintf(line, sizeof(line), "%s pc %08x lr %08x msr %08x, %llu "
		"instructions", cpu->IsHeldInReset() ? "held in reset"
			: cpu->IsHalted() ? "halted" : "running", cpu->pc, cpu->fLr,
		cpu->GetMsr(), (unsigned long long)cpu->InstructionCount());
	return line;
}


bool
PowerMacMachine::DebugTranslate(int cpu, u32 va, u32& pa)
{
	return fCpus[cpu]->DebugTranslate(va, false, pa);
}

}	// namespace powermac
