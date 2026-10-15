/*
 * PowerMac emulator for Haiku - the machine window.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Menu bar, the emulated screen and a status line. While the window is
 * active all keys go to the emulated ADB keyboard except the host key
 * (right Control), which toggles the mouse capture.
 *
 * Scripting (hey PowerMac ... of Window 0):
 *   get/set Running (bool), get Speed, Mips, EmulatedTime (double),
 *   get DiskActivity (bool), get Pointer (the guest's pointer),
 *   get SerialLog [1-2] (console output of the modem and printer ports),
 *   get/set Cd (the image in the CD-ROM drive, "" ejects),
 *   get/set Floppy (the image in the floppy drive, "" ejects),
 *   get/set Profile (the machine profile; set switches, restarting a
 *   running machine), get Profiles (all names)
 *   pwon, pwof, rset: power on, off, reset; pkey: the power key
 *   rhld with keys=...: restarts holding keys ("Command+Option+P+R")
 *   type with text=...: types on the emulated keyboard ("\n" is Return)
 *   shot [with path=...]: saves a PNG of the screen (replies "path")
 * Replies come after the action.
 */
#ifndef PM_MACHINE_WINDOW_H
#define PM_MACHINE_WINDOW_H

#include "HaikuAttributes.h"
#include "HaikuAudio.h"
#include "Settings.h"
#include "core/host/EmulatorThread.h"
#include "core/host/NetworkBackend.h"
#include "core/host/SerialBackend.h"

#include <Messenger.h>
#include <String.h>

#include <memory>
#include <vector>

#include <Window.h>

class BFilePanel;
class BMenu;
class ConsoleSerial;
class BMenuItem;
class BStringView;
class MediaStatusView;
class ScreenView;

namespace powermac {
class SharedVolume;
}

class MachineWindow : public BWindow {
public:
								MachineWindow(Settings& settings);
	virtual						~MachineWindow();

			bool				PowerOn();
			void				PowerOff();
			// Restarts a running machine with the profile. 'create' makes
			// a missing profile as a copy of the current one.
			bool				SwitchProfile(const char* name, bool create);

	virtual	void				DispatchMessage(BMessage* message,
									BHandler* handler);
	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();
	virtual	void				WindowActivated(bool active);
	virtual	void				FrameMoved(BPoint origin);
	virtual	void				FrameResized(float width, float height);

	virtual	BHandler*			ResolveSpecifier(BMessage* message,
									int32 index, BMessage* specifier,
									int32 what, const char* property);
	virtual	status_t			GetSupportedSuites(BMessage* data);

private:
			BMenu*				_BuildMachineMenu();
			void				_BuildProfileMenu();
			void				_NewProfile(const char* name);
			void				_RenameProfile(const char* name);
			void				_DeleteProfile();
			void				_ApplySettings();
			void				_ClosePreferences();
			BMenu*				_BuildMediaMenu();
			BMenu*				_BuildViewMenu();
			bool				_HandleKey(BMessage* message);
			int32				_HostKeyIndex() const;
			void				_ModifiersChanged(uint32 old, uint32 now);
			void				_SendKey(uint32 haikuKey, bool down);
			void				_SendAdbKey(int code, bool down);
			void				_ReleaseAllKeys();
			void				_UpdateStatus();
			void				_UpdateMenus();
			void				_FitWindow();
			bool				_RestartHolding(const char* keys,
									double seconds);
			void				_ShowConsole(int port);
			BString				_SaveScreenshot(const char* path = nullptr);
			bool				_HandleScripting(BMessage* message);
			// The CD-ROM drive media go to: the first one configured.
			bool				_CdDrive(int& bus, int& id) const;
			powermac::MachineConfig::ScsiTarget* _CdTarget();
			void				_InsertCd(const char* path);
			void				_EjectCd();
			void				_InsertFloppy(const char* path);
			void				_EjectFloppy();
			void				_PrepareSharedFolders(
									powermac::MachineConfig& config);
			void				_SyncSharedFolders(bool interactive);
			void				_TypeText(const char* text);

			Settings&			fSettings;
			// declared first: outlive the emulation thread using them
			HaikuAudio			fAudio;
			ConsoleSerial*		fConsoles[2];
			std::vector<std::unique_ptr<powermac::SerialBackend> >
								fSerialBackends;
			BMessenger			fConsoleWindows[2];
			std::unique_ptr<powermac::NetworkBackend> fNetwork;
			HaikuAttributes		fHostAttributes;
			std::vector<std::unique_ptr<powermac::SharedVolume> >
								fSharedFolders;
			powermac::EmulatorThread fEmulator;
			ScreenView*			fScreen;
			BStringView*		fStatus;
			MediaStatusView*	fMedia;
			BMenuItem*			fPowerItem;
			BMenuItem*			fPowerKeyItem;
			BMenuItem*			fPauseItem;
			BMenuItem*			fInsertCdItem;
			BMenuItem*			fEjectCdItem;
			BMenuItem*			fReinsertCdItem;
			BString				fLastCd;		// the disc ejected last
			BMenuItem*			fEjectFloppyItem;
			BMenu*				fProfileMenu;
			BMessenger			fPrefsWindow;
			BMenuItem*			fScaleItems[3];
			BFilePanel*			fCdPanel;
			BFilePanel*			fFloppyPanel;
			bool				fKeysDown[128];
			uint32				fModifiers;
			bool				fPowered;
			// the system has the machine on (it shuts it down itself)
			bool				fSystemOn;
};

#endif	// PM_MACHINE_WINDOW_H
