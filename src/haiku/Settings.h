/*
 * PowerMac emulator for Haiku - persistent settings and machine profiles.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * In ~/config/settings/PowerMac: "settings" (a flattened BMessage) holds
 * what all profiles share (keyboard and mouse, the window) and the current
 * profile's name; each profile is a folder in Profiles/ with the machine's
 * own settings, NVRAM and PRAM.
 */
#ifndef PM_SETTINGS_H
#define PM_SETTINGS_H

#include "core/board/MachineConfig.h"

#include <Rect.h>
#include <String.h>
#include <StringList.h>

class BMessage;

// Keys that release the mouse (and are not passed to the Mac).
enum HostKey {
	kHostKeyRightControl = 0,
	kHostKeyRightCommand = 1,
	kHostKeyRightOption = 2,
	kHostKeyMenu = 3
};

class Settings {
public:
								Settings();

			// The shared settings and the current profile.
			status_t			Load();
			status_t			Save() const;

	static	BString				Directory();
	static	BString				ProfilesDirectory();
			BString				ProfileDirectory() const;
			// Fills the paths derived from the settings directory.
			powermac::MachineConfig MachineConfigForRun() const;
			BString				NvramPath() const;
			BString				PramPath() const;

	// Profiles
	static	void				GetProfiles(BStringList& names);
	static	bool				ProfileExists(const char* name);
			// Why a name cannot be used for a new profile ("" if it can).
	static	BString				CheckProfileName(const char* name);
			// Saves the current profile and loads 'name'.
			status_t			SwitchProfile(const char* name);
			// A copy of the current profile (its settings, NVRAM and PRAM)
			// named 'name', which becomes the current one. Files already
			// in the new profile's folder are kept.
			status_t			CopyProfile(const char* name);
			status_t			RenameProfile(const char* name);
			// Deletes another profile.
	static	status_t			DeleteProfile(const char* name);

			// A disk or shared folder without an image would stop the
			// machine from starting: such targets become "none" (a CD-ROM
			// drive may be empty).
	static	void				DropEmptyDevices(powermac::MachineConfig& machine);

			// The clock: "" follows the host's; otherwise "YYYY-MM-DD
			// HH:MM" (local time) at every power on. False if 'text' is
			// neither.
	static	bool				ParseClock(const char* text,
									powermac::u32& macTime);

			BString				profile;

			// The profile's settings
			powermac::MachineConfig machine;
			BString				romPath;		// the 4 MB ROM
			BString				cardRomPath;	// the Twin Turbo 128's ROM
			BString				clock;
			int32				speedLimit;		// percent, 0 = unlimited
			bool				soundEnabled;
			int32				soundVolume;	// percent
			// Serial port destinations (modem, printer): "" (not
			// connected), "console", "tcp-listen:[ADDR:]PORT",
			// "tcp-connect:HOST:PORT", "host:DEVICE", "file:PATH".
			BString				serial[2];
			// Ethernet (MACE) on the user mode network: the address
			// ("00:05:02:..."), host port forwards ("tcp:2121:21,...").
			bool				ethernet;
			BString				macAddress;
			BString				portForwards;

			// Shared by all profiles
			int32				scale;			// 1, 2, 0 = fit window
			int32				hostKey;		// HostKey: releases the mouse
			int32				mouseSpeed;		// percent
			bool				captureOnClick;
			BRect				windowFrame;

private:
			void				_ResetProfile();
			void				_ReadProfile(const BMessage& archive);
			void				_WriteProfile(BMessage& archive) const;
			status_t			_LoadProfile();
			status_t			_SaveProfile() const;
};

#endif	// PM_SETTINGS_H
