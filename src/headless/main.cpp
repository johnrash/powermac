/*
 * PowerMac emulator - headless runner (Linux and Haiku).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Runs the machine without a user interface for development and automated
 * tests: serial ports to files or stdout, CPU states logged, optional
 * instruction traces, memory dumps and watches, screenshots.
 */
#include "core/Log.h"
#include "core/board/PowerMacMachine.h"
#include "core/bus/PhysBus.h"
#include "core/cpu/PpcCpu.h"
#include "core/debug/PngWriter.h"
#include "core/macio/Mace.h"
#include "core/host/AudioSink.h"
#include "core/host/KeyCodes.h"
#include "core/host/MacTime.h"
#include "core/host/NetworkBackend.h"
#include "core/host/SerialBackend.h"
#include "core/host/SerialBackends.h"
#include "core/host/VideoSink.h"
#include "core/storage/SharedFolder.h"
#include "core/storage/SharedMacFolder.h"
#include "core/video/DisplayDevice.h"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using namespace powermac;

namespace {


// Serial ports written to stdout and the runner's status lines share
// stdout: a status line waits for the end of the serial line in progress.
static bool sSerialMidLine = false;
static std::string sPendingStatus;


static void
Status(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (sSerialMidLine)
		sPendingStatus += line;
	else
		fputs(line, stdout);
}

class FileSerial : public SerialBackend {
public:
	FileSerial(FILE* file, const char* label, bool prefix,
			Scheduler& scheduler)
		: fFile(file), fLabel(label), fPrefix(prefix), fAtLineStart(true),
		fScheduler(scheduler) {}

	// Input typed from emulated time 'ticks', a character every 'gap'
	// ticks (Open Firmware drops what it gets faster than it echoes).
	void Queue(u64 ticks, const std::string& text, u64 gap)
	{
		for (char c : text) {
			fInput.push_back(std::make_pair(ticks, (u8)c));
			ticks += gap;
		}
	}

	virtual int Receive()
	{
		if (fInput.empty() || fScheduler.Now() < fInput.front().first)
			return -1;
		u8 byte = fInput.front().second;
		fInput.pop_front();
		return byte;
	}

	virtual void Transmit(u8 byte)
	{
		if (fPrefix && fAtLineStart)
			fprintf(fFile, "[%s] ", fLabel);
		fputc(byte, fFile);
		fAtLineStart = byte == '\n';
		if (fFile == stdout) {
			sSerialMidLine = !fAtLineStart;
			if (!sSerialMidLine && !sPendingStatus.empty()) {
				fputs(sPendingStatus.c_str(), stdout);
				sPendingStatus.clear();
			}
		}
		fflush(fFile);
	}

private:
	FILE* fFile;
	const char* fLabel;
	bool fPrefix;
	bool fAtLineStart;
	Scheduler& fScheduler;
	std::deque<std::pair<u64, u8> > fInput;
};


// Writes the sound output to a WAV file (at the first sample rate seen).
class WavSink : public AudioSink {
public:
	WavSink(const std::string& path) : fFile(fopen(path.c_str(), "wb")),
		fRate(0), fFrames(0), fPeak(0)
	{
		if (fFile != nullptr) {
			u8 header[44] = { 0 };
			fwrite(header, 1, sizeof(header), fFile);
		}
	}

	~WavSink()
	{
		if (fFile == nullptr)
			return;
		// RIFF header
		u32 dataBytes = (u32)(fFrames * 4);
		u8 h[44];
		memcpy(h, "RIFF", 4);
		StoreLE32(h + 4, 36 + dataBytes);
		memcpy(h + 8, "WAVEfmt ", 8);
		StoreLE32(h + 16, 16);
		StoreLE16(h + 20, 1);
		StoreLE16(h + 22, 2);
		StoreLE32(h + 24, fRate);
		StoreLE32(h + 28, fRate * 4);
		StoreLE16(h + 32, 4);
		StoreLE16(h + 34, 16);
		memcpy(h + 36, "data", 4);
		StoreLE32(h + 40, dataBytes);
		fseek(fFile, 0, SEEK_SET);
		fwrite(h, 1, sizeof(h), fFile);
		fclose(fFile);
		printf("audio: %llu frames at %u Hz, peak %d\n",
			(unsigned long long)fFrames, fRate, fPeak);
	}

	virtual void Play(const s16* frames, size_t count, u32 sampleRate)
	{
		if (fFile == nullptr)
			return;
		if (fRate == 0)
			fRate = sampleRate;
		for (size_t i = 0; i < count * 2; i++) {
			u8 bytes[2];
			StoreLE16(bytes, (u16)frames[i]);
			fwrite(bytes, 1, 2, fFile);
			int magnitude = frames[i] < 0 ? -frames[i] : frames[i];
			if (magnitude > fPeak)
				fPeak = magnitude;
		}
		fFrames += count;
	}

private:
	FILE* fFile;
	u32 fRate;
	u64 fFrames;
	int fPeak;
};


// Write watchpoint: the page is remapped read-direct with writes going
// through here, which logs the writer of the watched word.
class WriteWatch : public MmioDevice {
public:
	WriteWatch(PhysBus& bus, u32 address, u64 ticksPerSecond,
			Scheduler& scheduler)
		: fBus(bus), fAddress(address), fTicksPerSecond(ticksPerSecond),
		fScheduler(scheduler), fPage(nullptr) {}

	void Install()
	{
		u32 base = fAddress & ~PhysBus::kPageMask;
		fPage = fBus.Page(base).write;
		if (fPage == nullptr) {
			fprintf(stderr, "watch %08x: not in RAM\n", fAddress);
			return;
		}
		fBus.MapMemory(base, PhysBus::kPageSize, fPage, false, this);
	}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u8* p = fPage + (addr & PhysBus::kPageMask);
		return size == 1 ? *p : size == 2 ? LoadBE16(p) : LoadBE32(p);
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u8* p = fPage + (addr & PhysBus::kPageMask);
		if (addr < (fAddress & ~3u) + 4 && addr + size > (fAddress & ~3u)) {
			fprintf(stderr, "[%11.6f] watch write%d %08x = %08x by %s%d pc "
				"%08x\n", (double)fScheduler.Now() / fTicksPerSecond, size * 8, addr,
				value, initiator < PhysBus::kMaxCpus ? "cpu" : "dma",
				initiator, fBus.CurrentPc(initiator));
		}
		if (size == 1)
			*p = (u8)value;
		else if (size == 2)
			StoreBE16(p, (u16)value);
		else
			StoreBE32(p, value);
	}

	virtual const char* MmioName() const { return "watch"; }

private:
	PhysBus& fBus;
	u32 fAddress;
	u64 fTicksPerSecond;
	Scheduler& fScheduler;
	u8* fPage;
};


class TraceHook : public PpcTraceHook {
public:
	TraceHook(FILE* file, u64 limit, u64 ticksPerSecond) : fFile(file),
		fRemaining(limit), fStart(0), fCpuMask(3),
		fTicksPerSecond(ticksPerSecond) {}

	void SetStart(u64 ticks) { fStart = ticks; }
	void SetCpuMask(int mask) { fCpuMask = mask; }
	void AddPc(u32 pc) { fPcs.insert(pc); }
	void SetAllRegisters(bool all) { fAllRegisters = all; }
	void AddOpcode(u32 opcode) { fOpcodes.insert(opcode); }

	virtual void Instruction(PpcCpu& cpu, u32 pc, u32 instruction)
	{
		if (fRemaining == 0 || cpu.Now() < fStart
			|| (fCpuMask & (1 << cpu.Index())) == 0
			|| (!fPcs.empty() && fPcs.count(pc) == 0)
			|| (!fOpcodes.empty() && fOpcodes.count(instruction) == 0))
			return;
		fRemaining--;
		if (!fPcs.empty() || !fOpcodes.empty()) {
			fprintf(fFile, "[%11.6f] ",
				(double)cpu.Now() / fTicksPerSecond);
		}
		fprintf(fFile, "cpu%d %08x: %08x r0 %08x r1 %08x r2 %08x r3 %08x "
			"r4 %08x lr %08x msr %08x\n", cpu.Index(), pc, instruction,
			cpu.gpr[0], cpu.gpr[1], cpu.gpr[2], cpu.gpr[3], cpu.gpr[4],
			cpu.fLr, cpu.GetMsr());
		if (fAllRegisters) {
			for (int i = 0; i < 32; i++) {
				fprintf(fFile, "%sr%-2d %08x", i % 8 == 0 ? "    " : " ", i,
					cpu.gpr[i]);
				if (i % 8 == 7)
					fprintf(fFile, "\n");
			}
		}
	}

private:
	FILE* fFile;
	u64 fRemaining;
	u64 fStart;
	int fCpuMask;
	u64 fTicksPerSecond;
	std::set<u32> fPcs;
	std::set<u32> fOpcodes;
	bool fAllRegisters = false;
};


class FrameCapture : public VideoSink {
public:
	std::vector<u32> pixels;
	int width = 0;
	int height = 0;
	u64 frames = 0;

	virtual void FrameReady(const u32* data, int w, int h, int stride)
	{
		width = w;
		height = h;
		pixels.resize((size_t)w * h);
		for (int y = 0; y < h; y++)
			memcpy(&pixels[(size_t)y * w], data + (size_t)y * stride, w * 4);
		frames++;
	}
};


// The shared folders' volumes, in 'directory'; the machine gets them as
// disks (a Mac one takes its driver from one of the disks).
bool
PrepareShares(MachineConfig& config, const std::string& directory,
	std::vector<std::unique_ptr<SharedVolume> >& shares)
{
	MachineConfig::ScsiTarget* buses[2] = { config.scsiInternal,
		config.scsiExternal };
	std::vector<std::string> disks;
	for (int bus = 0; bus < 2; bus++) {
		for (int id = 0; id < 7; id++) {
			if (buses[bus][id].kind == MachineConfig::ScsiTarget::kDisk)
				disks.push_back(buses[bus][id].image);
		}
	}
	for (int bus = 0; bus < 2; bus++) {
		for (int id = 0; id < 7; id++) {
			MachineConfig::ScsiTarget& target = buses[bus][id];
			bool mac = target.kind == MachineConfig::ScsiTarget::kSharedMac;
			if (target.kind != MachineConfig::ScsiTarget::kSharedBe && !mac)
				continue;
			std::string image = directory + (mac ? "/powermac-shared-mac-"
				: "/powermac-shared-") + (bus == 0 ? "int" : "ext")
				+ std::to_string(id) + ".qcow2";
			std::unique_ptr<SharedVolume> share;
			if (mac)
				share.reset(new SharedMacFolder(target.image, image, disks));
			else
				share.reset(new SharedFolder(target.image, image));
			std::string report;
			std::string error;
			if (!share->Prepare(report, error)) {
				fprintf(stderr, "shared folder %s: %s\n",
					target.image.c_str(), error.c_str());
				return false;
			}
			if (!report.empty())
				printf("shared folder: %s\n", report.c_str());
			target.image = image;
			target.readOnly = false;
			shares.push_back(std::move(share));
		}
	}
	return true;
}


void
Usage()
{
	printf("usage: powermac-headless [options]\n"
		"  --rom FILE          the 4 MB ROM (default: $POWERMAC_ROM)\n"
		"  --model NAME        9500/120, /132, /150, /180MP (default), /200\n"
		"  --ram MB            memory (default 64)\n"
		"  --cpus N            processors (1 or 2; default: the model's)\n"
		"  --threads           each processor on a host thread of its own\n"
		"  --nvram FILE        NVRAM (8 KB) to load and save\n"
		"  --pram FILE         Cuda's PRAM and clock to load and save\n"
		"  --time YYYY-MM-DD[THH:MM]  the clock at power on\n"
		"  --card-rom FILE     the video card's expansion ROM (default:\n"
		"                      $POWERMAC_CARD_ROM; \"\" for none)\n"
		"  --monitor N         the monitor's sense code (0: 1024x768, ...)\n"
		"  --scsi-int-N-disk FILE, --scsi-int-N-cd FILE  a target (ID N)\n"
		"                      on the internal bus (MESH)\n"
		"  --scsi-ext-N-disk FILE, --scsi-ext-N-cd FILE  on the external\n"
		"                      bus (53C94)\n"
		"  --scsi-int-N-share-mac FOLDER, --scsi-int-N-share-be FOLDER\n"
		"                      a host folder as an HFS (Mac OS) or BFS\n"
		"                      (BeOS) disk, synced back at the end\n"
		"  --share-dir DIR     where shared folders' volumes are made\n"
		"                      (default $TMPDIR or /tmp)\n"
		"  --floppy FILE       a disk in the floppy drive\n"
		"  --net               Ethernet (MACE) on user mode networking\n"
		"  --net-forward SPEC  forward host ports: [tcp:|udp:]HOST:GUEST,...\n"
		"  --seconds S         emulated seconds to run (default 5)\n"
		"  --serial-a DEST, --serial-b DEST  the modem / printer port:\n"
		"                      stdout (default for A), null, a file,\n"
		"                      tcp-listen:[ADDR:]PORT, tcp-connect:HOST:PORT,\n"
		"                      host:DEVICE, file:PATH\n"
		"  --input A|B:MS:TEXT type TEXT into a serial port from MS, a\n"
		"                      character every 3 ms (\\r, \\n, \\t)\n"
		"  --log CAT=LEVEL     log level (error, warning, info, debug,\n"
		"                      trace) of a category or \"all\"\n"
		"  --trace N           trace N instructions to stderr\n"
		"  --trace-from MS     start the trace at MS\n"
		"  --trace-cpus MASK   CPUs to trace (bit 0: CPU 0)\n"
		"  --trace-pc ADDR     only these instructions, with time stamps\n"
		"  --trace-regs        all 32 registers in the trace\n"
		"  --trace-op HEX      only instructions with this encoding (Mac OS's\n"
		"                      68K emulator fetches with af780002: r24 is the\n"
		"                      68K PC)\n"
		"  --status MS         the CPUs' states every MS\n"
		"  --dump ADDR:LEN:FILE  physical memory at the end\n"
		"  --dump-va ADDR:LEN:FILE  virtual memory (CPU 0) at the end\n"
		"  --regs              the CPUs' MMU and special registers at the end\n"
		"  --watch ADDR        report changes of a word\n"
		"  --watch-write ADDR[@MS]  report who writes a word (from MS)\n"
		"  --key MS:KEYS[:HOLD] press keys for HOLD ms (default 100): names\n"
		"                      (Command, Option, Control, Shift, Return,\n"
		"                      A, F1, ...) or 0xNN ADB codes, joined by +\n"
		"                      (Command+Option+O+F)\n"
		"  --type MS:TEXT      type ASCII text from MS (\\n Return, \\t Tab);\n"
		"                      keys wait for the mouse actions before them\n"
		"  --click MS:X,Y      from MS on, move the mouse until the pointer is\n"
		"                      at X,Y, then click; mouse actions run in order.\n"
		"                      The pointer: the RAMDAC's cursor when on, else\n"
		"                      Mac OS's (the low memory global Mouse)\n"
		"  --dclick MS:X,Y     double click\n"
		"  --insert-cd MS:FILE put a CD image into the CD-ROM drive at MS (the\n"
		"                      systems eject it themselves; \"\" empties it)\n"
		"  --insert-floppy MS:FILE  put a disk image into the floppy drive\n"
		"                      at MS (\"\" ejects it)\n"
		"  --next-floppy FILE  the next disk to put in, 2 s after the system\n"
		"                      ejected one (repeat for several, in order)\n"
		"  --press MS:X,Y      move and keep the button pressed\n"
		"  --release MS:X,Y    move (with the button held) and release\n"
		"  --screenshot FILE   the screen at the end (PNG)\n"
		"  --shots PREFIX      a PNG every second (PREFIX-N.png)\n"
		"  --audio FILE        the sound output (WAV)\n");
}


LogLevel
ParseLevel(const char* name)
{
	static const char* const kNames[] = { "error", "warning", "info",
		"debug", "trace" };
	for (int i = 0; i < 5; i++) {
		if (strcmp(name, kNames[i]) == 0)
			return (LogLevel)i;
	}
	return kLogWarning;
}


// "YYYY-MM-DD[THH:MM]" as Macintosh time (local time, seconds since 1904).
bool
ParseMacTime(const char* text, u32& macTime)
{
	int year, month, day, hour = 0, minute = 0;
	int fields = sscanf(text, "%d-%d-%dT%d:%d", &year, &month, &day, &hour,
		&minute);
	if (fields != 3 && fields != 5)
		return false;
	return MacTimeFromDate(year, month, day, hour, minute, macTime);
}


std::string
Unescape(const char* text)
{
	std::string result;
	for (const char* p = text; *p != '\0'; p++) {
		if (*p == '\\' && p[1] != '\0') {
			p++;
			result += *p == 'r' ? '\r' : *p == 'n' ? '\n' : *p == 't' ? '\t'
				: *p;
		} else
			result += *p;
	}
	return result;
}

}	// namespace


int
main(int argc, char** argv)
{
	MachineConfig config;
	// the ROMs from the environment unless given
	if (const char* rom = getenv("POWERMAC_ROM"))
		config.romPath = rom;
	if (const char* cardRom = getenv("POWERMAC_CARD_ROM"))
		config.cardRomPath = cardRom;
	double seconds = 5;
	u64 traceCount = 0;
	double traceFromMs = 0;
	int traceCpus = 3;
	bool traceRegisters = false;
	std::vector<u32> traceOpcodes;
	std::vector<u32> tracePcs;
	double statusMs = 0;
	std::string screenshot;
	std::string shots;
	std::string audioPath;
	std::string netForwards;
	const char* temporary = getenv("TMPDIR");
	std::string shareDirectory = temporary != nullptr ? temporary : "/tmp";
	struct Dump {
		u32 address;
		u32 length;
		std::string file;
		bool virt;
	};
	std::vector<Dump> dumps;
	std::vector<u32> watches;
	bool printRegisters = false;
	u32 writeWatchAddress = 0;
	double writeWatchMs = -1;
	struct KeyEvent {
		u64 time;		// microseconds
		u8 code;
		bool down;
		bool done;
	};
	std::vector<KeyEvent> keys;
	struct MouseAction {
		u64 time;		// microseconds
		int x;
		int y;
		enum { kClick, kPress, kRelease } kind;
	};
	std::vector<MouseAction> mouseActions;
	struct CdInsert {
		u64 time;		// microseconds
		std::string path;
		bool done;
	};
	std::vector<CdInsert> cdInserts;
	std::vector<CdInsert> floppyInserts;
	std::deque<std::string> nextFloppies;
	bool floppyWasIn = false;
	u64 floppyEjectedUs = 0;
	std::string serial[2] = { "stdout", "null" };
	struct SerialInput {
		int port;
		double ms;
		std::string text;
	};
	std::vector<SerialInput> serialInputs;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		auto next = [&]() -> const char* {
			if (i + 1 >= argc) {
				fprintf(stderr, "missing value for %s\n", arg.c_str());
				exit(2);
			}
			return argv[++i];
		};
		int scsiId;
		char busName[4];
		char kindName[16];
		if (arg == "--rom")
			config.romPath = next();
		else if (arg == "--model") {
			const char* name = next();
			if (!MachineConfig::ModelFromName(name, config.model)) {
				fprintf(stderr, "unknown model %s\n", name);
				return 2;
			}
		} else if (arg == "--ram")
			config.ramMB = atoi(next());
		else if (arg == "--cpus")
			config.cpuCount = atoi(next());
		else if (arg == "--threads")
			config.threadPerCpu = true;
		else if (arg == "--nvram")
			config.nvramPath = next();
		else if (arg == "--pram")
			config.pramPath = next();
		else if (arg == "--time") {
			const char* text = next();
			if (!ParseMacTime(text, config.fixedTime)) {
				fprintf(stderr, "bad time %s\n", text);
				return 2;
			}
		} else if (arg == "--card-rom")
			config.cardRomPath = next();
		else if (arg == "--monitor")
			config.monitor = atoi(next());
		else if (sscanf(arg.c_str(), "--scsi-%3[a-z]-%d-%15[a-z-]", busName,
				&scsiId, kindName) == 3 && scsiId >= 0 && scsiId < 7
			&& (strcmp(busName, "int") == 0 || strcmp(busName, "ext") == 0)) {
			MachineConfig::ScsiTarget& target = strcmp(busName, "int") == 0
				? config.scsiInternal[scsiId] : config.scsiExternal[scsiId];
			target.image = next();
			if (strcmp(kindName, "cd") == 0)
				target.kind = MachineConfig::ScsiTarget::kCdRom;
			else if (strcmp(kindName, "disk") == 0)
				target.kind = MachineConfig::ScsiTarget::kDisk;
			else if (strcmp(kindName, "share-mac") == 0)
				target.kind = MachineConfig::ScsiTarget::kSharedMac;
			else if (strcmp(kindName, "share-be") == 0
				|| strcmp(kindName, "share") == 0)
				target.kind = MachineConfig::ScsiTarget::kSharedBe;
			else {
				fprintf(stderr, "unknown option %s\n", arg.c_str());
				return 2;
			}
		} else if (arg == "--share-dir")
			shareDirectory = next();
		else if (arg == "--floppy")
			config.floppyImage = next();
		else if (arg == "--net")
			config.ethernet = true;
		else if (arg == "--net-forward") {
			config.ethernet = true;
			netForwards = next();
		}
		else if (arg == "--seconds")
			seconds = atof(next());
		else if (arg == "--serial-a")
			serial[0] = next();
		else if (arg == "--serial-b")
			serial[1] = next();
		else if (arg == "--input") {
			const char* spec = next();
			char port;
			double ms;
			int consumed;
			if (sscanf(spec, "%c:%lf:%n", &port, &ms, &consumed) != 2
				|| (port != 'A' && port != 'B' && port != 'a' && port != 'b')) {
				fprintf(stderr, "bad input %s\n", spec);
				return 2;
			}
			serialInputs.push_back({ (port == 'A' || port == 'a') ? 0 : 1,
				ms, Unescape(spec + consumed) });
		} else if (arg == "--log") {
			std::string spec = next();
			size_t equals = spec.find('=');
			if (equals == std::string::npos) {
				fprintf(stderr, "bad log spec %s\n", spec.c_str());
				return 2;
			}
			std::string name = spec.substr(0, equals);
			LogLevel level = ParseLevel(spec.c_str() + equals + 1);
			LogCategory category;
			if (name == "all")
				Log::SetAllLevels(level);
			else if (Log::CategoryFromName(name.c_str(), category))
				Log::SetLevel(category, level);
			else {
				fprintf(stderr, "unknown log category %s\n", name.c_str());
				return 2;
			}
		} else if (arg == "--trace")
			traceCount = strtoull(next(), nullptr, 0);
		else if (arg == "--trace-from")
			traceFromMs = atof(next());
		else if (arg == "--trace-cpus")
			traceCpus = atoi(next());
		else if (arg == "--trace-pc") {
			tracePcs.push_back((u32)strtoul(next(), nullptr, 16));
			if (traceCount == 0)
				traceCount = ~0ull;
		} else if (arg == "--trace-op") {
			traceOpcodes.push_back((u32)strtoul(next(), nullptr, 16));
			if (traceCount == 0)
				traceCount = ~0ull;
		} else if (arg == "--trace-regs")
			traceRegisters = true;
		else if (arg == "--status")
			statusMs = atof(next());
		else if (arg == "--dump" || arg == "--dump-va") {
			const char* spec = next();
			unsigned address, length;
			char file[512];
			if (sscanf(spec, "%x:%x:%511s", &address, &length, file) != 3) {
				fprintf(stderr, "bad dump spec %s\n", spec);
				return 2;
			}
			dumps.push_back({ address, length, file, arg == "--dump-va" });
		} else if (arg == "--regs")
			printRegisters = true;
		else if (arg == "--watch")
			watches.push_back((u32)strtoul(next(), nullptr, 16));
		else if (arg == "--watch-write") {
			const char* spec = next();
			writeWatchAddress = (u32)strtoul(spec, nullptr, 16);
			const char* at = strchr(spec, '@');
			writeWatchMs = at != nullptr ? atof(at + 1) : 0;
		} else if (arg == "--key") {
			std::string spec = next();
			size_t colon = spec.find(':');
			if (colon == std::string::npos) {
				fprintf(stderr, "bad key spec %s\n", spec.c_str());
				return 2;
			}
			double ms = atof(spec.c_str());
			std::string names = spec.substr(colon + 1);
			double hold = 100;
			size_t holdColon = names.find(':');
			if (holdColon != std::string::npos) {
				hold = atof(names.c_str() + holdColon + 1);
				names.resize(holdColon);
			}
			std::vector<u8> codes;
			size_t start = 0;
			while (start <= names.size()) {
				size_t plus = names.find('+', start);
				if (plus == std::string::npos)
					plus = names.size();
				std::string name = names.substr(start, plus - start);
				int code = name.compare(0, 2, "0x") == 0
					? (int)strtol(name.c_str(), nullptr, 16)
					: KeyNameToAdb(name.c_str());
				if (code < 0 || code > 0x7f) {
					fprintf(stderr, "unknown key %s\n", name.c_str());
					return 2;
				}
				codes.push_back((u8)code);
				start = plus + 1;
			}
			// pressed in order 10 ms apart, released in reverse
			for (size_t k = 0; k < codes.size(); k++) {
				keys.push_back({ (u64)((ms + 10 * k) * 1000), codes[k], true,
					false });
				keys.push_back({ (u64)((ms + hold + 10 * (codes.size() - k))
					* 1000), codes[k], false, false });
			}
		} else if (arg == "--insert-cd" || arg == "--insert-floppy") {
			std::string spec = next();
			size_t colon = spec.find(':');
			if (colon == std::string::npos) {
				fprintf(stderr, "bad %s spec %s\n", arg.c_str() + 9,
					spec.c_str());
				return 2;
			}
			(arg == "--insert-cd" ? cdInserts : floppyInserts).push_back(
				{ (u64)(atof(spec.c_str()) * 1000), spec.substr(colon + 1),
					false });
		} else if (arg == "--next-floppy")
			nextFloppies.push_back(next());
		else if (arg == "--type") {
			std::string spec = next();
			size_t colon = spec.find(':');
			if (colon == std::string::npos) {
				fprintf(stderr, "bad type spec %s\n", spec.c_str());
				return 2;
			}
			double ms = atof(spec.c_str());
			std::string text = Unescape(spec.substr(colon + 1).c_str());
			int shift = KeyNameToAdb("Shift");
			for (char c : text) {
				bool shifted;
				int code = AsciiToAdb(c, shifted);
				if (code < 0)
					continue;
				// a key 30 ms down, 30 ms apart (Shift around it)
				if (shifted)
					keys.push_back({ (u64)(ms * 1000), (u8)shift, true, false });
				keys.push_back({ (u64)((ms + 10) * 1000), (u8)code, true,
					false });
				keys.push_back({ (u64)((ms + 40) * 1000), (u8)code, false,
					false });
				if (shifted) {
					keys.push_back({ (u64)((ms + 50) * 1000), (u8)shift, false,
						false });
				}
				ms += 80;
			}
		} else if (arg == "--click" || arg == "--press"
			|| arg == "--release" || arg == "--dclick") {
			std::string value = next();
			MouseAction action{ 0, 0, 0, MouseAction::kClick };
			if (arg == "--press")
				action.kind = MouseAction::kPress;
			else if (arg == "--release")
				action.kind = MouseAction::kRelease;
			double ms;
			if (sscanf(value.c_str(), "%lf:%d,%d", &ms, &action.x,
					&action.y) < 3) {
				fprintf(stderr, "bad click %s\n", value.c_str());
				return 2;
			}
			action.time = (u64)(ms * 1000);
			mouseActions.push_back(action);
			if (arg == "--dclick") {
				action.time = 0;
				mouseActions.push_back(action);
			}
		} else if (arg == "--screenshot")
			screenshot = next();
		else if (arg == "--shots")
			shots = next();
		else if (arg == "--audio")
			audioPath = next();
		else {
			if (arg != "--help")
				fprintf(stderr, "unknown option %s\n", arg.c_str());
			Usage();
			return arg == "--help" ? 0 : 2;
		}
	}

	if (config.romPath.empty()) {
		fprintf(stderr, "powermac-headless: no ROM: give --rom FILE or set "
			"POWERMAC_ROM\n");
		return 2;
	}

	std::string error;
	// shared folders: their volumes, built now and synced at the end
	std::vector<std::unique_ptr<SharedVolume> > shares;
	if (!PrepareShares(config, shareDirectory, shares))
		return 1;
	std::unique_ptr<PowerMacMachine> owner(new PowerMacMachine(config));
	PowerMacMachine& machine = *owner;
	if (!machine.Init(error)) {
		fprintf(stderr, "powermac-headless: %s\n", error.c_str());
		return 1;
	}

	std::vector<std::unique_ptr<FileSerial> > backends;
	std::vector<std::unique_ptr<SerialBackend> > otherBackends;
	static const char* const kLabels[] = { "modem", "printer" };
	for (int port = 0; port < 2; port++) {
		const std::string& destination = serial[port];
		if (destination == "null")
			continue;
		if (destination.compare(0, 4, "tcp-") == 0
			|| destination.compare(0, 5, "host:") == 0
			|| destination.compare(0, 5, "file:") == 0) {
			std::unique_ptr<SerialBackend> backend
				= CreateSerialBackend(destination, error);
			if (!backend) {
				fprintf(stderr, "%s port: %s\n", kLabels[port],
					error.c_str());
				return 1;
			}
			machine.SetSerialBackend(port, backend.get());
			otherBackends.push_back(std::move(backend));
			continue;
		}
		FILE* file = destination == "stdout" ? stdout
			: fopen(destination.c_str(), "w");
		if (file == nullptr) {
			perror(destination.c_str());
			return 1;
		}
		backends.emplace_back(new FileSerial(file, kLabels[port],
			destination == "stdout", machine.GetScheduler()));
		machine.SetSerialBackend(port, backends.back().get());
		for (const SerialInput& input : serialInputs) {
			if (input.port == port) {
				backends.back()->Queue((u64)(input.ms
					* machine.TicksPerSecond() / 1000), input.text,
					machine.MicrosecondsToTicks(3000));
			}
		}
	}

	FrameCapture capture;
	machine.SetVideoSink(&capture);
	std::unique_ptr<WavSink> wav;
	if (!audioPath.empty()) {
		wav.reset(new WavSink(audioPath));
		machine.SetAudioSink(wav.get());
	}

	std::unique_ptr<NetworkBackend> network;
	if (config.ethernet) {
		UserNetworkConfig networkConfig;
		if (!ParsePortForwards(netForwards, networkConfig.forwards, error)) {
			fprintf(stderr, "--net-forward: %s\n", error.c_str());
			return 2;
		}
		network = CreateUserNetwork(networkConfig, error);
		if (!network) {
			fprintf(stderr, "--net: %s\n", error.c_str());
			return 1;
		}
		machine.SetNetworkBackend(network.get());
	}

	TraceHook trace(stderr, traceCount, machine.TicksPerSecond());
	for (u32 pc : tracePcs)
		trace.AddPc(pc);
	trace.SetStart((u64)(traceFromMs * machine.TicksPerSecond() / 1000));
	trace.SetCpuMask(traceCpus);
	trace.SetAllRegisters(traceRegisters);
	for (u32 opcode : traceOpcodes)
		trace.AddOpcode(opcode);
	if (traceCount > 0) {
		for (int i = 0; i < machine.CpuCount(); i++)
			machine.Cpu(i)->SetTraceHook(&trace);
	}

	auto start = std::chrono::steady_clock::now();
	u64 total = (u64)(seconds * machine.TicksPerSecond());
	u64 step = machine.MicrosecondsToTicks(1000);	// 1 ms
	u64 statusEvery = statusMs > 0
		? machine.MicrosecondsToTicks((u64)(statusMs * 1000)) : 0;
	u64 nextStatus = statusEvery;
	u64 done = 0;
	std::vector<u32> watchValues;
	for (u32 address : watches)
		watchValues.push_back(machine.Bus().Read32(address, kInitiatorDma));
	std::unique_ptr<WriteWatch> writeWatch;
	bool wasPoweredOn = true;
	// mouse script state
	size_t mouseIndex = 0;
	enum { kMouseIdle, kMouseMoving, kMousePressed, kMouseReleased }
		mousePhase = kMouseIdle;
	u64 mouseNext = 0;
	int mouseStuck = 0;
	u32 mouseButtons = 0;
	while (done < total) {
		u64 nowUs = machine.Now() * 1000000 / machine.TicksPerSecond();
		if (writeWatchMs >= 0 && !writeWatch && nowUs >= writeWatchMs * 1000) {
			writeWatch.reset(new WriteWatch(machine.Bus(), writeWatchAddress,
				machine.TicksPerSecond(), machine.GetScheduler()));
			writeWatch->Install();
		}
		u64 chunk = std::min(step, total - done);
		machine.RunFor(chunk);
		done += chunk;
		nowUs = machine.Now() * 1000000 / machine.TicksPerSecond();
		for (KeyEvent& key : keys) {
			if (key.done || nowUs < key.time)
				continue;
			// keys come after the mouse actions given before them: those
			// take as long as the pointer needs, the keys then move on
			if (mouseIndex < mouseActions.size()
				&& mouseActions[mouseIndex].time <= key.time) {
				u64 late = nowUs - key.time;
				u64 from = key.time;
				for (KeyEvent& later : keys) {
					if (!later.done && later.time >= from)
						later.time += late + 1000;
				}
				continue;
			}
			if (key.down)
				machine.KeyDown(key.code);
			else
				machine.KeyUp(key.code);
			key.done = true;
		}
		for (CdInsert& insert : cdInserts) {
			if (insert.done || nowUs < insert.time)
				continue;
			insert.done = true;
			// the first CD-ROM drive: external bus first (ID 3 on a 9500)
			int bus = -1, id = -1;
			for (int b = 1; b >= 0 && bus < 0; b--) {
				const MachineConfig::ScsiTarget* targets = b == 0
					? config.scsiInternal : config.scsiExternal;
				for (int t = 0; t < 7; t++) {
					if (targets[t].kind == MachineConfig::ScsiTarget::kCdRom) {
						bus = b;
						id = t;
						break;
					}
				}
			}
			std::string error;
			if (bus < 0)
				fprintf(stderr, "--insert-cd: no CD-ROM drive\n");
			else if (insert.path.empty())
				machine.EjectCd(bus, id);
			else if (!machine.InsertCd(bus, id, insert.path, error))
				fprintf(stderr, "--insert-cd: %s\n", error.c_str());
			else
				Status("[%9.3f ms] CD inserted: %s\n", nowUs / 1000.0,
					insert.path.c_str());
		}
		for (CdInsert& insert : floppyInserts) {
			if (insert.done || nowUs < insert.time)
				continue;
			insert.done = true;
			std::string error;
			if (insert.path.empty())
				machine.EjectFloppy();
			else if (!machine.InsertFloppy(insert.path,
					config.floppyWriteProtected, error))
				fprintf(stderr, "--insert-floppy: %s\n", error.c_str());
			else
				Status("[%9.3f ms] floppy inserted: %s\n", nowUs / 1000.0,
					insert.path.c_str());
		}
		if (!nextFloppies.empty()) {
			bool in = !machine.FloppyPath().empty();
			if (floppyWasIn && !in)
				floppyEjectedUs = nowUs;
			floppyWasIn = in;
			if (!in && floppyEjectedUs != 0
				&& nowUs >= floppyEjectedUs + 2000000) {
				std::string path = nextFloppies.front();
				nextFloppies.pop_front();
				std::string error;
				if (!machine.InsertFloppy(path, config.floppyWriteProtected,
						error))
					fprintf(stderr, "--next-floppy: %s\n", error.c_str());
				else
					Status("[%9.3f ms] floppy inserted: %s\n",
						nowUs / 1000.0, path.c_str());
				floppyEjectedUs = 0;
				floppyWasIn = true;
			}
		}
		if (mouseIndex < mouseActions.size() && nowUs >= mouseNext) {
			const MouseAction& action = mouseActions[mouseIndex];
			int cx, cy;
			switch (mousePhase) {
				case kMouseIdle:
					if (nowUs >= action.time)
						mousePhase = kMouseMoving;
					break;
				case kMouseMoving:
					if (!machine.PointerPosition(cx, cy)) {
						mouseNext = nowUs + 20000;
						break;
					}
					if (cx == action.x && cy == action.y) {
						if (action.kind == MouseAction::kRelease) {
							mouseButtons = 0;
							mousePhase = kMouseReleased;
						} else {
							mouseButtons = 1;
							mousePhase = action.kind == MouseAction::kPress
								? kMouseReleased : kMousePressed;
						}
						machine.MouseMoved(0, 0, mouseButtons);
						mouseStuck = 0;
						mouseNext = nowUs + 60000;
						break;
					}
					if (++mouseStuck > 400) {
						fprintf(stderr, "mouse: cannot reach %d,%d (at %d,%d)"
							"\n", action.x, action.y, cx, cy);
						mousePhase = kMouseReleased;
						mouseStuck = 0;
						break;
					}
					{
						// small steps: the guest accelerates large ones
						int dx = std::max(-6, std::min(6, action.x - cx));
						int dy = std::max(-6, std::min(6, action.y - cy));
						machine.MouseMoved(dx, dy, mouseButtons);
					}
					mouseNext = nowUs + 20000;
					break;
				case kMousePressed:
					mouseButtons = 0;
					machine.MouseMoved(0, 0, 0);
					mousePhase = kMouseReleased;
					mouseNext = nowUs + 60000;
					break;
				case kMouseReleased:
					Status("[%9.3f ms] mouse at %d,%d%s\n", nowUs / 1000.0,
						action.x, action.y,
						action.kind == MouseAction::kPress ? " pressed"
						: action.kind == MouseAction::kRelease ? " released"
						: " clicked");
					mouseIndex++;
					mousePhase = kMouseIdle;
					break;
			}
		}
		for (size_t i = 0; i < watches.size(); i++) {
			u32 value = machine.Bus().Read32(watches[i], kInitiatorDma);
			if (value != watchValues[i]) {
				Status("[%9.3f ms] watch %08x: %08x -> %08x\n",
					nowUs / 1000.0, watches[i], watchValues[i], value);
				watchValues[i] = value;
			}
		}
		if (machine.IsPoweredOn() != wasPoweredOn) {
			wasPoweredOn = machine.IsPoweredOn();
			Status("[%9.3f ms] power %s\n", nowUs / 1000.0,
				wasPoweredOn ? "on" : "off");
		}
		if (!shots.empty() && done % machine.TicksPerSecond() < step
			&& capture.width > 0) {
			std::string path = shots + "-" + std::to_string(
				done / machine.TicksPerSecond()) + ".png";
			WritePng(path, capture.pixels.data(), capture.width,
				capture.height, capture.width);
		}
		if (statusEvery != 0 && done >= nextStatus) {
			nextStatus += statusEvery;
			for (int i = 0; i < machine.CpuCount(); i++) {
				Status("[%9.3f ms] cpu%d %s\n",
					machine.Now() * 1000.0 / machine.TicksPerSecond(), i,
					machine.CpuState(i).c_str());
			}
		}
	}
	double elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - start).count();

	if (!sPendingStatus.empty()) {
		printf("\n%s", sPendingStatus.c_str());
		sPendingStatus.clear();
	}
	printf("emulated %.3f s in %.3f s (%.0f%%), %.1f MIPS\n",
		machine.Now() / (double)machine.TicksPerSecond(), elapsed,
		100.0 * machine.Now() / machine.TicksPerSecond() / elapsed,
		machine.InstructionCount() / elapsed / 1e6);
	for (int i = 0; i < machine.CpuCount(); i++)
		printf("cpu%d: %s\n", i, machine.CpuState(i).c_str());
	for (int i = 0; printRegisters && i < machine.CpuCount(); i++) {
		PpcCpu& cpu = *machine.Cpu(i);
		static const struct { int spr; const char* name; } kSprs[] = {
			{ SPR_SDR1, "sdr1" }, { SPR_HID0, "hid0" }, { SPR_SRR0, "srr0" },
			{ SPR_SRR1, "srr1" }, { SPR_DAR, "dar" }, { SPR_DSISR, "dsisr" },
			{ SPR_SPRG0, "sprg0" }, { SPR_SPRG1, "sprg1" },
			{ SPR_SPRG2, "sprg2" }, { SPR_SPRG3, "sprg3" },
			{ SPR_DEC, "dec" }, { SPR_PIR, "pir" }
		};
		printf("cpu%d registers:", i);
		for (const auto& entry : kSprs) {
			u32 value;
			if (cpu.ReadSpr(entry.spr, value))
				printf(" %s %08x", entry.name, value);
		}
		printf("\n  bats:");
		for (int bat = 0; bat < 8; bat++) {
			u32 upper, lower;
			cpu.ReadSpr(SPR_IBAT0U + 2 * bat, upper);
			cpu.ReadSpr(SPR_IBAT0U + 2 * bat + 1, lower);
			printf(" %s%d %08x/%08x", bat < 4 ? "i" : "d", bat & 3, upper,
				lower);
			if (bat == 3)
				printf("\n       ");
		}
		printf("\n  srs:");
		for (int sr = 0; sr < 16; sr++)
			printf(" %08x", cpu.GetSr(sr));
		printf("\n  gprs:");
		for (int r = 0; r < 32; r++)
			printf("%s %08x", r % 8 == 0 && r > 0 ? "\n       " : "", cpu.gpr[r]);
		printf("\n");
	}
	for (const Dump& dump : dumps) {
		std::vector<u8> data(dump.length);
		if (dump.virt) {
			// page by page through CPU0's translation (unmapped: zeroes)
			for (u32 offset = 0; offset < dump.length;) {
				u32 va = dump.address + offset;
				u32 chunk = std::min<u32>(dump.length - offset,
					0x1000 - (va & 0xfff));
				u32 pa;
				if (machine.DebugTranslate(0, va, pa))
					machine.Bus().DmaRead(pa, data.data() + offset, chunk);
				offset += chunk;
			}
		} else
			machine.Bus().DmaRead(dump.address, data.data(), dump.length);
		FILE* file = fopen(dump.file.c_str(), "wb");
		if (file == nullptr
			|| fwrite(data.data(), 1, data.size(), file) != data.size())
			fprintf(stderr, "cannot write %s\n", dump.file.c_str());
		if (file != nullptr)
			fclose(file);
	}
	if (!screenshot.empty() && capture.width > 0) {
		WritePng(screenshot, capture.pixels.data(), capture.width,
			capture.height, capture.width);
		printf("screenshot %dx%d saved to %s (%llu frames)\n", capture.width,
			capture.height, screenshot.c_str(),
			(unsigned long long)capture.frames);
	}
	int cursorX, cursorY;
	if (machine.Display() != nullptr
		&& machine.Display()->HardwareCursor(cursorX, cursorY))
		printf("hardware cursor at %d,%d\n", cursorX, cursorY);
	if (config.ethernet) {
		printf("ethernet: %llu frames sent, %llu received\n",
			(unsigned long long)machine.Ethernet().FramesSent(),
			(unsigned long long)machine.Ethernet().FramesReceived());
	}
	if (!config.floppyImage.empty() || !floppyInserts.empty()) {
		std::string floppy = machine.FloppyPath();
		printf("floppy drive: %s\n", floppy.empty() ? "empty"
			: floppy.c_str());
	}
	printf("unclaimed accesses: %llu memory, %llu I/O\n",
		(unsigned long long)machine.Bus().UnclaimedAccessCount(),
		(unsigned long long)machine.UnclaimedIoCount());
	if (!machine.SavePersistentState(error))
		fprintf(stderr, "powermac-headless: %s\n", error.c_str());
	// the disks closed, the shared volumes go back to their folders
	owner.reset();
	for (std::unique_ptr<SharedVolume>& share : shares) {
		std::string report;
		if (!share->Sync(report, error))
			printf("shared folder: %s\n", error.c_str());
		else
			printf("shared folder: %s\n", report.empty()
				? "no changes" : report.c_str());
	}
	return 0;
}
