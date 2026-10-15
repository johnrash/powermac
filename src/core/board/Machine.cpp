/*
 * PowerMac emulator - an emulated machine, as the front ends see it.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Machine.h"

#include "PowerMacMachine.h"

#include <cstring>

namespace powermac {

std::unique_ptr<Machine>
Machine::Create(const MachineConfig& config)
{
	return std::unique_ptr<Machine>(new PowerMacMachine(config));
}


// The Power Macintosh 9500 family. The 604e's PLL configuration (HID1)
// is the ratio's: 3:1 or 4:1.
static const MachineConfig::ModelInfo kModels[MachineConfig::kModelCount] = {
	{ "9500/120", false, 1, 120000000, 40000000, 0x00040103, 0 },
	{ "9500/132", false, 1, 132000000, 44000000, 0x00040103, 0 },
	{ "9500/150", true, 1, 150000000, 50000000, 0x00090202, 0x80000000 },
	{ "9500/180MP", true, 2, 180000000, 45000000, 0x00090202, 0xa0000000 },
	{ "9500/200", true, 1, 200000000, 50000000, 0x00090202, 0xa0000000 }
};


const MachineConfig::ModelInfo&
MachineConfig::Info(Model model)
{
	return kModels[model < kModelCount ? model : k9500_180MP];
}


bool
MachineConfig::ModelFromName(const char* name, Model& model)
{
	for (int i = 0; i < kModelCount; i++) {
		const char* full = kModels[i].name;
		if (strcasecmp(name, full) == 0 || strcasecmp(name, full + 5) == 0) {
			model = (Model)i;
			return true;
		}
	}
	return false;
}

}	// namespace powermac
