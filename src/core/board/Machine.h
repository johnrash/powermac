/*
 * PowerMac emulator - an emulated machine, as the front ends see it.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Time is counted in machine ticks, whose rate depends on the model
 * (TicksPerSecond()). All methods run on the emulation thread.
 */
#ifndef PM_MACHINE_H
#define PM_MACHINE_H

#include "MachineConfig.h"
#include "../Types.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace powermac {

class AudioSink;
class DisplayDevice;
class NetworkBackend;
class PhysBus;
class Scheduler;
class SerialBackend;
class VideoSink;

class Machine {
public:
	// A PowerMacMachine.
	static	std::unique_ptr<Machine> Create(const MachineConfig& config);

	virtual						~Machine() {}

	virtual	bool				Init(std::string& error) = 0;
	virtual	const MachineConfig& Config() const = 0;

			// Power on / hard reset (the power key while off, the reset
			// switch of the programmer's switch).
	virtual	void				PowerOn() = 0;
			// Whether the machine runs: it powers itself off when the
			// system shuts down (Cuda).
	virtual	bool				IsPoweredOn() const = 0;
			// The interrupt switch of the programmer's switch (NMI).
	virtual	void				PressInterruptSwitch() = 0;
			// Saves the NVRAM and the PRAM if they changed.
	virtual	bool				SavePersistentState(std::string& error) = 0;
			// Keeps the NVRAM and PRAM in other files from now on (a
			// profile copied while the machine runs); the next save writes
			// both there.
	virtual	void				SetPersistentPaths(const std::string& nvram,
									const std::string& pram) = 0;

			// Advances the emulation by 'ticks' machine ticks.
	virtual	void				RunFor(u64 ticks) = 0;
	virtual	u64					Now() const = 0;
	virtual	u64					TicksPerSecond() const = 0;
			u64					MicrosecondsToTicks(u64 us) const
									{ return us * TicksPerSecond()
										/ 1000000; }
	virtual	u64					InstructionCount() const = 0;

	virtual	PhysBus&			Bus() = 0;
	virtual	Scheduler&			GetScheduler() = 0;

			// Input: ADB key codes (an Apple Extended Keyboard: see
			// host/KeyCodes.h) and the mouse (relative motion, +y down;
			// buttons: bit 0 the button).
	virtual	void				KeyDown(u8 adbCode) = 0;
	virtual	void				KeyUp(u8 adbCode) = 0;
	virtual	void				MouseMoved(int dx, int dy, u32 buttons) = 0;
			// Keys pressed and released in order, 'interval' ticks apart
			// (typing, key combinations): ADB code, down.
	virtual	void				QueueKeys(
									const std::vector<std::pair<u8, bool> >&
										events, u64 interval) = 0;
			// Presses the keys now and releases them after 'ticks' (keys
			// held through the start: C, Shift, Command-Option-P-R, ...).
	virtual	void				HoldKeys(const std::vector<u8>& codes,
									u64 ticks) = 0;

			// The CD-ROM drive at a SCSI target (bus 0: internal, 1:
			// external). Inserting fails when no drive is there; the path
			// is "" when the drive is empty (the system ejects too).
	virtual	bool				InsertCd(int bus, int id,
									const std::string& path,
									std::string& error) = 0;
	virtual	void				EjectCd(int bus, int id) = 0;
	virtual	std::string			CdPath(int bus, int id) const = 0;

			// The floppy drive: a 720 KB or 1.44 MB image. The path is ""
			// when the drive is empty (the system ejects disks itself).
	virtual	bool				InsertFloppy(const std::string& path,
									bool writeProtected,
									std::string& error) = 0;
	virtual	void				EjectFloppy() = 0;
	virtual	std::string			FloppyPath() const = 0;

			// The display card (null when there is none).
	virtual	DisplayDevice*		Display() const = 0;
			// Where the guest's pointer is on the screen: the card's
			// hardware cursor when it shows one, else Mac OS's (the low
			// memory global Mouse); false when neither is known.
	virtual	bool				PointerPosition(int& x, int& y) = 0;

	virtual	int					SerialPortCount() const = 0;
	virtual	void				SetSerialBackend(int port,
									SerialBackend* backend) = 0;
	virtual	void				SetNetworkBackend(NetworkBackend* backend)
									= 0;
	virtual	bool				HasEthernet() const = 0;
	virtual	void				SetVideoSink(VideoSink* sink) = 0;
	virtual	void				SetAudioSink(AudioSink* sink) = 0;

			// Disk activity (duty cycle 0-1) since the last call.
	virtual	float				SampleDiskActivity() = 0;
	virtual	bool				CpuActive(int cpu) const = 0;
			// Diagnostics: accesses nothing claimed; the processors.
	virtual	u64					UnclaimedIoCount() const = 0;
	virtual	int					CpuCount() const = 0;
			// One line: running, halted or held in reset, the program
			// counter and the main control registers.
	virtual	std::string			CpuState(int cpu) const = 0;
			// The physical address of a virtual one, as the CPU sees it.
	virtual	bool				DebugTranslate(int cpu, u32 va, u32& pa) = 0;
};

}	// namespace powermac

#endif	// PM_MACHINE_H
