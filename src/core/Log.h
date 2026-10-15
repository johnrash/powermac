/*
 * PowerMac emulator - logging.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_LOG_H
#define PM_LOG_H

#include "Types.h"

namespace powermac {

enum LogCategory {
	kLogCpu = 0,
	kLogMmu,
	kLogBus,		// unclaimed accesses
	kLogBoard,		// Hammerhead, board registers
	kLogPci,		// Bandit, configuration cycles
	kLogInterrupt,	// Grand Central's interrupt controller
	kLogTimer,		// VIA timers
	kLogDma,		// DBDMA
	kLogSerial,		// ESCC
	kLogCuda,		// Cuda: ADB, real time clock, PRAM, power
	kLogNvram,
	kLogFloppy,		// SWIM3
	kLogScsi,		// MESH, 53C94, the targets
	kLogVideo,
	kLogAudio,		// AWACS
	kLogNetwork,	// MACE
	kLogStorage,
	kLogMachine,
	kLogHost,
	kLogCategoryCount
};

enum LogLevel {
	kLogError = 0,
	kLogWarning,
	kLogInfo,
	kLogDebug,
	kLogTrace
};

typedef void (*LogSink)(LogCategory category, LogLevel level,
	const char* message, void* cookie);

class Log {
public:
	static void SetSink(LogSink sink, void* cookie);
	static void SetLevel(LogCategory category, LogLevel level);
	static void SetAllLevels(LogLevel level);
	static LogLevel Level(LogCategory category)
		{ return sLevels[category]; }
	static bool Enabled(LogCategory category, LogLevel level)
		{ return level <= sLevels[category]; }
	static void Write(LogCategory category, LogLevel level,
		const char* format, ...) __attribute__((format(printf, 3, 4)));
	static const char* CategoryName(LogCategory category);
	static bool CategoryFromName(const char* name, LogCategory& category);

private:
	static LogLevel sLevels[kLogCategoryCount];
	static LogSink sSink;
	static void* sCookie;
};

}	// namespace powermac

#define PM_LOG(cat, lvl, ...) \
	do { \
		if (powermac::Log::Enabled(powermac::cat, powermac::lvl)) \
			powermac::Log::Write(powermac::cat, powermac::lvl, __VA_ARGS__); \
	} while (0)

#endif	// PM_LOG_H
