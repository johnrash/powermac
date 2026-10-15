/*
 * PowerMac emulator - the second processor on a host thread of its own.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "SmpRunner.h"

#include "Scheduler.h"
#include "../Spin.h"
#include "../bus/PhysBus.h"
#include "../cpu/PpcCpu.h"

namespace powermac {

SmpRunner::SmpRunner(PhysBus& bus, Scheduler& scheduler, PpcCpu& cpu0,
	PpcCpu& cpu1)
	:
	fBus(bus),
	fScheduler(scheduler),
	fCpu0(cpu0),
	fCpu1(cpu1),
	fSliceEnd(0),
	fStart(0),
	fDone(0),
	fSleeping(false),
	fQuit(false)
{
	fBus.SetThreaded(true);
	fThread = std::thread([this]() { _Run(); });
}


SmpRunner::~SmpRunner()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
	}
	fWake.notify_one();
	fThread.join();
	fBus.SetThreaded(false);
}


void
SmpRunner::_Run()
{
	fScheduler.SetClock(fCpu1.NowPointer());
	u64 seen = 0;
	while (true) {
		u64 generation = 0;
		for (int spins = 0;; spins++) {
			generation = fStart.load(std::memory_order_acquire);
			if (generation != seen || fQuit)
				break;
			if (spins < 2000) {
				CpuRelax();
				continue;
			}
			// idle (the emulation paused or paced): sleep
			std::unique_lock<std::mutex> lock(fLock);
			fSleeping = true;
			fWake.wait(lock, [&]() {
				return fStart.load() != seen || fQuit;
			});
			fSleeping = false;
			spins = 0;
		}
		if (fQuit)
			break;
		seen = generation;
		fCpu1.Run(fSliceEnd);
		fDone.store(generation, std::memory_order_release);
	}
	fScheduler.SetClock(nullptr);
}


void
SmpRunner::RunSlice(u64 sliceEnd)
{
	fSliceEnd = sliceEnd;
	u64 generation = fStart.load() + 1;
	fStart.store(generation);
	if (fSleeping) {
		std::lock_guard<std::mutex> lock(fLock);
		fWake.notify_one();
	}
	fScheduler.SetClock(fCpu0.NowPointer());
	fCpu0.Run(sliceEnd);
	fScheduler.SetClock(nullptr);
	for (int spins = 0; fDone.load(std::memory_order_acquire) != generation;
			spins++) {
		if (spins < 100000)
			CpuRelax();
		else
			std::this_thread::yield();
	}
}

}	// namespace powermac
