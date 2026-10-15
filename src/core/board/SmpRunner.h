/*
 * PowerMac emulator - the second processor on a host thread of its own.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Threaded mode (MachineConfig::threadPerCpu): a worker thread runs CPU 1
 * through the time slices the machine starts (generation numbers),
 * spinning between them for a while, then sleeping; CPU 0 runs on the
 * thread calling RunSlice(). Both meet at the end of each slice, where
 * the devices run. Taken from BeBoxMachine.
 */
#ifndef PM_SMP_RUNNER_H
#define PM_SMP_RUNNER_H

#include "../Types.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace powermac {

class PhysBus;
class PpcCpu;
class Scheduler;

class SmpRunner {
public:
								SmpRunner(PhysBus& bus, Scheduler& scheduler,
									PpcCpu& cpu0, PpcCpu& cpu1);
								~SmpRunner();

			// Both CPUs run to 'sliceEnd' at the same time.
			void				RunSlice(u64 sliceEnd);

private:
			void				_Run();

			PhysBus&			fBus;
			Scheduler&			fScheduler;
			PpcCpu&				fCpu0;
			PpcCpu&				fCpu1;
			std::thread			fThread;
			u64					fSliceEnd;
			std::atomic<u64>	fStart;		// slice generation to run
			std::atomic<u64>	fDone;		// last generation run
			std::atomic<bool>	fSleeping;
			std::atomic<bool>	fQuit;
			std::mutex			fLock;
			std::condition_variable fWake;
};

}	// namespace powermac

#endif	// PM_SMP_RUNNER_H
