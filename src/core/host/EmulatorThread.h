/*
 * PowerMac emulator - runs a Machine on its own thread in real time.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * The machine is only touched by the emulation thread. Front ends post
 * commands (run on that thread between time slices) and read a status
 * snapshot.
 */
#ifndef PM_EMULATOR_THREAD_H
#define PM_EMULATOR_THREAD_H

#include "../board/MachineConfig.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace powermac {

class Machine;

class EmulatorThread {
public:
	typedef std::function<void(Machine&)> Command;

	struct Status {
		bool running = false;
		bool paused = false;
		double speedPercent = 0;	// emulated vs real time
		double mips = 0;
		bool poweredOn = false;		// the system did not shut down
		float disk = 0;				// disk activity duty cycle
		bool cpuActive[2] = { false, false };
		// the guest's hardware cursor (-1: none shown)
		int cursorX = -1;
		int cursorY = -1;
		double emulatedSeconds = 0;
	};

								EmulatorThread();
								~EmulatorThread();

			// Creates the machine and starts the thread. 'setup' runs on
			// the emulation thread after Machine::Init() (attach sinks and
			// backends there).
			bool				Start(const MachineConfig& config,
									std::string& error,
									Command setup = nullptr);
			// Stops the thread and saves the persistent state.
			void				Stop();
			bool				IsRunning() const { return fThread.joinable(); }

			void				Post(Command command);
			void				SetPaused(bool paused);
			bool				IsPaused() const { return fPaused; }
			// 100 = real time, 0 = as fast as possible.
			void				SetSpeedLimit(int percent)
									{ fSpeedLimit = percent; }

			Status				GetStatus();

private:
			void				_Run();

			std::unique_ptr<Machine> fMachine;
			std::thread			fThread;
			std::mutex			fLock;
			std::condition_variable fCondition;
			std::vector<Command> fCommands;
			std::atomic<bool>	fQuit;
			std::atomic<bool>	fPaused;
			std::atomic<int>	fSpeedLimit;
			Status				fStatus;
			std::mutex			fStatusLock;
};

}	// namespace powermac

#endif	// PM_EMULATOR_THREAD_H
