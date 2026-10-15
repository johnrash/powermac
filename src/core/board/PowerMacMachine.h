/*
 * PowerMac emulator - the Power Macintosh 9500: processors, buses and
 * devices.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Time is counted in machine ticks, 12 per bus clock (540 MHz with the
 * 9500/180MP's 45 MHz bus): a processor cycle is 3 ticks at 4:1 and 4 at
 * 3:1, a time base/decrementer tick (bus / 4) 48 ticks.
 *
 * Physical map: RAM from 0 (Hammerhead's banks), PCI memory at
 * 0x80000000-0xEFFFFFFF, the Bandits at 0xF2000000 and 0xF4000000, Grand
 * Central at 0xF3000000 (where the ROM puts it), Hammerhead at
 * 0xF8000000, the ROM at 0xFFC00000.
 *
 * All methods run on the emulation thread.
 */
#ifndef PM_POWER_MAC_MACHINE_H
#define PM_POWER_MAC_MACHINE_H

#include "Machine.h"
#include "MachineConfig.h"
#include "Scheduler.h"
#include "../Types.h"

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace powermac {

class Awacs;
class Bandit;
class Cuda;
class Escc;
class GrandCentral;
class Hammerhead;
class ImsTwinTurbo128;
class Mesh;
class Ncr53c94;
class PpcCpu;
class ScsiCd;
class SmpRunner;
class Mace;
class Swim3;
class Via6522;

class PowerMacMachine : public Machine {
public:
	static const int kTicksPerBusCycle = 12;
	static const u32 kRomBase = 0xffc00000;
	static const u32 kRomSize = 0x400000;

	explicit					PowerMacMachine(const MachineConfig& config);
	virtual						~PowerMacMachine();

	virtual	bool				Init(std::string& error);
	virtual	const MachineConfig& Config() const { return fConfig; }

	virtual	void				PowerOn();
	virtual	bool				IsPoweredOn() const { return fPoweredOn; }
	virtual	void				PressInterruptSwitch();
	virtual	bool				SavePersistentState(std::string& error);
	virtual	void				SetPersistentPaths(const std::string& nvram,
									const std::string& pram);

	virtual	void				RunFor(u64 ticks);
	virtual	u64					Now() const { return fNow; }
	virtual	u64					TicksPerSecond() const
									{ return fTicksPerSecond; }
	virtual	u64					InstructionCount() const;

	virtual	PhysBus&			Bus() { return *fBus; }
	virtual	Scheduler&			GetScheduler() { return fScheduler; }

	virtual	void				KeyDown(u8 adbCode);
	virtual	void				KeyUp(u8 adbCode);
	virtual	void				MouseMoved(int dx, int dy, u32 buttons);
	virtual	void				QueueKeys(
									const std::vector<std::pair<u8, bool> >&
										events, u64 interval);
	virtual	void				HoldKeys(const std::vector<u8>& codes,
									u64 ticks);

	virtual	bool				InsertCd(int bus, int id,
									const std::string& path,
									std::string& error);
	virtual	void				EjectCd(int bus, int id);
	virtual	std::string			CdPath(int bus, int id) const;
	virtual	bool				InsertFloppy(const std::string& path,
									bool writeProtected, std::string& error);
	virtual	void				EjectFloppy();
	virtual	std::string			FloppyPath() const;

	virtual	DisplayDevice*		Display() const;
	virtual	bool				PointerPosition(int& x, int& y);

	virtual	int					SerialPortCount() const { return 2; }
	virtual	void				SetSerialBackend(int port,
									SerialBackend* backend);
	virtual	void				SetNetworkBackend(NetworkBackend* backend);
	virtual	bool				HasEthernet() const { return true; }
	virtual	void				SetVideoSink(VideoSink* sink);
	virtual	void				SetAudioSink(AudioSink* sink);

	virtual	float				SampleDiskActivity()
									{ return fDiskBusy.exchange(false)
										? 1 : 0; }
	virtual	bool				CpuActive(int cpu) const;
	virtual	u64					UnclaimedIoCount() const;
	virtual	int					CpuCount() const { return (int)fCpus.size(); }
	virtual	std::string			CpuState(int cpu) const;
	virtual	bool				DebugTranslate(int cpu, u32 va, u32& pa);

			// Components, for the tests and the headless runner.
			PpcCpu*				Cpu(int index)
									{ return index < (int)fCpus.size()
										? fCpus[index].get() : nullptr; }
			Hammerhead&			MemoryController() { return *fHammerhead; }
			GrandCentral&		IoController() { return *fGrandCentral; }
			Cuda&				SystemController() { return *fCuda; }
			Escc&				Serial() { return *fEscc; }
			Awacs&				Sound() { return *fAwacs; }
			Swim3&				Floppy() { return *fSwim3; }
			Mace&				Ethernet() { return *fMace; }
			u8*					Nvram();

private:
			bool				_LoadRom(std::string& error);
			bool				_SetupVideo(std::string& error);
			bool				_SetupScsi(std::string& error);
			void				_LoadPersistentState();
			void				_PowerOff();
			void				_RequestPowerOn();
			void				_WatchForNanokernel();
			void				_NextQueuedKey();
			ScsiCd*				_Cd(int bus, int id) const;
	static	u32					_HostMacTime();

			MachineConfig		fConfig;
			MachineConfig::ModelInfo fModel;
			bool				fSaveAll;		// next save: even if clean
			u64					fTicksPerSecond;
			u64					fQuantum;
			u64					fNow;
			bool				fPoweredOn;
			Scheduler			fScheduler;
			std::unique_ptr<PhysBus> fBus;
			std::unique_ptr<u8[]> fRam;
			std::unique_ptr<u8[]> fRom;
			std::unique_ptr<Hammerhead> fHammerhead;
			std::unique_ptr<Bandit> fBandit1;
			std::unique_ptr<Bandit> fBandit2;
			std::unique_ptr<GrandCentral> fGrandCentral;
			std::unique_ptr<Via6522> fVia;
			std::unique_ptr<Cuda> fCuda;
			std::unique_ptr<Escc> fEscc;
			std::unique_ptr<Awacs> fAwacs;
			std::unique_ptr<Swim3> fSwim3;
			std::unique_ptr<Mace> fMace;
			std::unique_ptr<ImsTwinTurbo128> fVideo;
			std::unique_ptr<Mesh> fMesh;
			std::unique_ptr<Ncr53c94> fScsiExternal;
			// disk activity: busy since the last sample
			std::atomic<bool>	fDiskBusy;
			std::vector<std::unique_ptr<PpcCpu> > fCpus;
			std::unique_ptr<SmpRunner> fSmp;
			// Watches CPU 0 leave Open Firmware (Hammerhead.h).
			std::unique_ptr<Timer> fNanokernelTimer;
			// Cuda's clock: the Macintosh time it has to the host's
			s64					fClockOffset;
			// QueueKeys(): what is left and the time between keys
			std::deque<std::pair<u8, bool> > fKeyQueue;
			u64					fKeyInterval;
			std::unique_ptr<Timer> fKeyTimer;
			// HoldKeys(): the keys to release
			std::vector<u8>		fHeldKeys;
			std::unique_ptr<Timer> fHoldTimer;
			// a reset asked for by a device while the CPUs run
			std::atomic<bool>	fPowerOnRequested;
};

}	// namespace powermac

#endif	// PM_POWER_MAC_MACHINE_H
