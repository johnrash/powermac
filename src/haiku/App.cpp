/*
 * PowerMac emulator for Haiku - the application.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Command line (also delivered to a running instance):
 *   PowerMac [--profile NAME]
 *         [--rom FILE] [--card-rom FILE] [--model 9500/180MP] [--ram MB]
 *         [--cpus N] [--clock "YYYY-MM-DD HH:MM"] [--net]
 *         [--net-forward SPEC] [--scsi-int-N-disk FILE]
 *         [--scsi-int-N-cd FILE] [--scsi-int-N-share-mac FOLDER]
 *         [--scsi-int-N-share-be FOLDER] [--scsi-ext-N-...] [--power-on]
 * (N is the SCSI ID, 0-6; an empty FILE removes the device; -share is
 * -share-be, as on the BeBox.) --profile
 * selects a machine profile (made as a copy of the current one if there is
 * none); the other options change that profile.
 */
#include "PowerMac.h"
#include "MachineWindow.h"
#include "Settings.h"

#include <AboutWindow.h>
#include <Application.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

class App : public BApplication {
public:
	App()
		:
		BApplication(kAppSignature),
		fWindow(nullptr),
		fPowerOn(false)
	{
		fSettings.Load();
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		for (int32 i = 1; i + 1 < argc; i++) {
			if (strcmp(argv[i], "--profile") == 0)
				_SelectProfile(argv[i + 1]);
		}
		for (int32 i = 1; i < argc; i++) {
			const char* arg = argv[i];
			const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
			if (strcmp(arg, "--power-on") == 0) {
				fPowerOn = true;
				continue;
			}
			if (strcmp(arg, "--net") == 0) {
				fSettings.ethernet = true;
				continue;
			}
			if (value == nullptr)
				break;
			powermac::MachineConfig::Model model;
			if (strcmp(arg, "--profile") == 0)
				;	// done first
			else if (strcmp(arg, "--rom") == 0)
				fSettings.romPath = value;
			else if (strcmp(arg, "--card-rom") == 0)
				fSettings.cardRomPath = value;
			else if (strcmp(arg, "--model") == 0) {
				if (powermac::MachineConfig::ModelFromName(value, model))
					fSettings.machine.model = model;
				else
					fprintf(stderr, "PowerMac: unknown model %s\n", value);
			} else if (strcmp(arg, "--ram") == 0)
				fSettings.machine.ramMB = atoi(value);
			else if (strcmp(arg, "--cpus") == 0)
				fSettings.machine.cpuCount = atoi(value);
			else if (strcmp(arg, "--clock") == 0)
				fSettings.clock = value;
			else if (strcmp(arg, "--net-forward") == 0) {
				fSettings.ethernet = true;
				fSettings.portForwards = value;
			} else if (_ScsiOption(arg, value))
				;
			else
				continue;
			i++;
		}
		if (fWindow != nullptr && fPowerOn)
			fWindow->PostMessage(kMsgPowerOn);
	}

	virtual void ReadyToRun()
	{
		fWindow = new MachineWindow(fSettings);
		fWindow->Show();
		if (fPowerOn)
			fWindow->PostMessage(kMsgPowerOn);
	}

	virtual void AboutRequested()
	{
		BAboutWindow* about = new BAboutWindow(kAppName, kAppSignature);
		about->AddDescription("An emulator of the Power Macintosh 9500/180MP: "
			"two PowerPC 604e processors, the 9500's logic board running "
			"Apple's own ROM and Open Firmware, and an IMS Twin Turbo 128 "
			"video card. It runs Mac OS and BeOS for the Macintosh.");
		const char* authors[] = { "The BeBox and PowerMac emulator authors",
			nullptr };
		about->AddAuthors(authors);
		about->AddCopyright(2026, "The BeBox and PowerMac emulator authors");
		about->AddText("Distributed under the terms of the MIT License.");
		about->Show();
	}

private:
	// --scsi-int-N-disk/-cd/-share-mac/-share-be, --scsi-ext-N-...
	bool _ScsiOption(const char* arg, const char* value)
	{
		bool internal = strncmp(arg, "--scsi-int-", 11) == 0;
		if (!internal && strncmp(arg, "--scsi-ext-", 11) != 0)
			return false;
		char id = arg[11];
		const char* kind = arg + 12;
		bool shareMac = strcmp(kind, "-share-mac") == 0;
		bool shareBe = strcmp(kind, "-share-be") == 0
			|| strcmp(kind, "-share") == 0;
		if (id < '0' || id > '6' || (strcmp(kind, "-disk") != 0
				&& strcmp(kind, "-cd") != 0 && !shareMac && !shareBe))
			return false;
		powermac::MachineConfig::ScsiTarget& target = internal
			? fSettings.machine.scsiInternal[id - '0']
			: fSettings.machine.scsiExternal[id - '0'];
		bool cd = strcmp(kind, "-cd") == 0;
		// an empty image removes the device (a CD-ROM drive: the disc)
		target.kind = value[0] == '\0' && !cd
			? powermac::MachineConfig::ScsiTarget::kNone
			: cd ? powermac::MachineConfig::ScsiTarget::kCdRom
			: shareMac ? powermac::MachineConfig::ScsiTarget::kSharedMac
			: shareBe ? powermac::MachineConfig::ScsiTarget::kSharedBe
			: powermac::MachineConfig::ScsiTarget::kDisk;
		target.image = value;
		target.readOnly = cd;
		return true;
	}

	void _SelectProfile(const char* name)
	{
		if (fWindow != nullptr) {
			if (fWindow->Lock()) {
				fWindow->SwitchProfile(name, true);
				fWindow->Unlock();
			}
			return;
		}
		status_t status = B_OK;
		if (Settings::ProfileExists(name))
			status = fSettings.SwitchProfile(name);
		else if (Settings::CheckProfileName(name).IsEmpty())
			status = fSettings.CopyProfile(name);
		else
			status = B_BAD_VALUE;
		if (status != B_OK)
			fprintf(stderr, "PowerMac: cannot use the profile \"%s\"\n", name);
	}

	Settings		fSettings;
	MachineWindow*	fWindow;
	bool			fPowerOn;
};


int
main()
{
	App app;
	app.Run();
	return 0;
}
