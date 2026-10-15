/*
 * PowerMac emulator for Haiku - the machine window.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "MachineWindow.h"

#include "PowerMac.h"
#include "MediaStatusView.h"
#include "PrefsWindow.h"
#include "ProfileNameWindow.h"
#include "ScreenView.h"
#include "SerialConsole.h"
#include "core/board/Machine.h"
#include "core/host/KeyCodes.h"
#include "core/host/SerialBackends.h"
#include "core/storage/DiskImage.h"
#include "core/storage/FloppyImage.h"
#include "core/storage/SharedFolder.h"
#include "core/storage/SharedMacFolder.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Directory.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PropertyInfo.h>
#include <StringList.h>
#include <StringView.h>
#include <TranslatorRoster.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using powermac::MachineConfig;

// Restart holding keys: the menu's choices and how long they are held
// (emulated seconds).
static const struct {
	const char* label;
	const char* keys;
	double seconds;
} kBootKeys[] = {
	{ "C (start up from the CD)", "C", 15 },
	{ "Shift (extensions off)", "Shift", 25 },
	{ "Command-Option-P-R (reset PRAM)", "Command+Option+P+R", 4 },
	{ "Command-Option-O-F (Open Firmware)", "Command+Option+O+F", 6 },
	{ "Command-Option-Shift-Delete (skip the startup disk)",
		"Command+Option+Shift+Delete", 15 }
};

static const uint32 kMsgTick = 'tick';
static const uint32 kMsgCdState = 'cdst';
static const uint32 kMsgFloppyState = 'flst';
// The host key toggles the mouse capture and is not passed to the Mac
// (key code, modifier bit or 0), by Settings::hostKey.
static const struct {
	uint32 key;
	uint32 modifier;
} kHostKeys[4] = {
	{ 0x60, B_RIGHT_CONTROL_KEY },
	{ 0x5f, B_RIGHT_COMMAND_KEY },		// right Alt
	{ 0x67, B_RIGHT_OPTION_KEY },		// right Windows key
	{ 0x68, 0 }							// Menu
};


MachineWindow::MachineWindow(Settings& settings)
	:
	BWindow(settings.windowFrame, kAppName, B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fSettings(settings),
	fCdPanel(nullptr),
	fFloppyPanel(nullptr),
	fModifiers(0),
	fPowered(false),
	fSystemOn(false)
{
	memset(fKeysDown, 0, sizeof(fKeysDown));
	for (int i = 0; i < 2; i++)
		fConsoles[i] = new ConsoleSerial(i);

	BMenuBar* menuBar = new BMenuBar("menu");
	menuBar->AddItem(_BuildMachineMenu());
	menuBar->AddItem(_BuildMediaMenu());
	menuBar->AddItem(_BuildViewMenu());
	BMenu* tools = new BMenu("Tools");
	BMenu* consoles = new BMenu("Serial console");
	static const char* const kPortNames[2] = { "Modem port (Open Firmware)",
		"Printer port" };
	for (int i = 0; i < 2; i++) {
		BMessage* message = new BMessage(kMsgSerialConsole);
		message->AddInt32("port", i);
		consoles->AddItem(new BMenuItem(kPortNames[i], message,
			i == 0 ? 'D' : 0));
	}
	tools->AddItem(consoles);
	tools->AddItem(new BMenuItem("Save screenshot",
		new BMessage(kMsgScreenshot), 'S', B_SHIFT_KEY));
	menuBar->AddItem(tools);

	fScreen = new ScreenView(fEmulator);
	fScreen->SetMouseOptions(fSettings.mouseSpeed, fSettings.captureOnClick);
	fScreen->SetScale(settings.scale);
	fStatus = new BStringView("status", "Powered off");
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fMedia = new MediaStatusView();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.Add(fScreen)
		.AddGroup(B_HORIZONTAL, 0)
			.SetInsets(B_USE_SMALL_SPACING, 2, B_USE_SMALL_SPACING, 2)
			.Add(fStatus)
			.Add(fMedia)
		.End();

	BMessage tick(kMsgTick);
	new BMessageRunner(BMessenger(this), &tick, 50000);
	_UpdateMenus();
}


MachineWindow::~MachineWindow()
{
	fEmulator.Stop();
	fNetwork.reset();
	_SyncSharedFolders(false);
	fSerialBackends.clear();
	for (int i = 0; i < 2; i++) {
		if (fConsoleWindows[i].IsValid()) {
			BLooper* looper;
			BWindow* window = dynamic_cast<BWindow*>(
				fConsoleWindows[i].Target(&looper));
			if (window != nullptr && window->Lock())
				window->Quit();
		}
		delete fConsoles[i];
	}
	delete fCdPanel;
	delete fFloppyPanel;
}


BMenu*
MachineWindow::_BuildMachineMenu()
{
	BMenu* menu = new BMenu("Machine");
	fPowerItem = new BMenuItem("Power on", new BMessage(kMsgPowerOn));
	menu->AddItem(fPowerItem);
	menu->AddItem(new BMenuItem("Reset", new BMessage(kMsgReset)));
	fPowerKeyItem = new BMenuItem("Power key", new BMessage(kMsgPowerKey));
	menu->AddItem(fPowerKeyItem);
	menu->AddItem(new BMenuItem("Interrupt (NMI)",
		new BMessage(kMsgInterruptSwitch)));
	menu->AddSeparatorItem();
	BMenu* holding = new BMenu("Restart holding");
	for (size_t i = 0; i < sizeof(kBootKeys) / sizeof(kBootKeys[0]); i++) {
		BMessage* message = new BMessage(kMsgRestartHolding);
		message->AddString("keys", kBootKeys[i].keys);
		message->AddDouble("seconds", kBootKeys[i].seconds);
		holding->AddItem(new BMenuItem(kBootKeys[i].label, message));
	}
	menu->AddItem(holding);
	menu->AddSeparatorItem();
	fPauseItem = new BMenuItem("Pause", new BMessage(kMsgPause));
	menu->AddItem(fPauseItem);
	menu->AddSeparatorItem();
	fProfileMenu = new BMenu("Profile");
	menu->AddItem(fProfileMenu);
	_BuildProfileMenu();
	menu->AddItem(new BMenuItem("Preferences" B_UTF8_ELLIPSIS,
		new BMessage(kMsgPreferences), ','));
	menu->AddItem(new BMenuItem("About PowerMac" B_UTF8_ELLIPSIS,
		new BMessage(B_ABOUT_REQUESTED)));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
	return menu;
}


// The profiles (the current one marked), then the commands for the
// current one.
void
MachineWindow::_BuildProfileMenu()
{
	fProfileMenu->RemoveItems(0, fProfileMenu->CountItems(), true);
	BStringList names;
	Settings::GetProfiles(names);
	for (int32 i = 0; i < names.CountStrings(); i++) {
		BMessage* message = new BMessage(kMsgSwitchProfile);
		message->AddString("name", names.StringAt(i));
		BMenuItem* item = new BMenuItem(names.StringAt(i).String(), message);
		item->SetMarked(names.StringAt(i) == fSettings.profile);
		fProfileMenu->AddItem(item);
	}
	fProfileMenu->AddSeparatorItem();
	fProfileMenu->AddItem(new BMenuItem("New profile" B_UTF8_ELLIPSIS,
		new BMessage(kMsgNewProfile)));
	BString label;
	label.SetToFormat("Rename \"%s\"" B_UTF8_ELLIPSIS,
		fSettings.profile.String());
	fProfileMenu->AddItem(new BMenuItem(label.String(),
		new BMessage(kMsgRenameProfile)));
	label.SetToFormat("Delete \"%s\"" B_UTF8_ELLIPSIS,
		fSettings.profile.String());
	BMenuItem* remove = new BMenuItem(label.String(),
		new BMessage(kMsgDeleteProfile));
	remove->SetEnabled(names.CountStrings() > 1);
	fProfileMenu->AddItem(remove);
	fProfileMenu->SetTargetForItems(this);
}


bool
MachineWindow::SwitchProfile(const char* name, bool create)
{
	if (fSettings.profile == name)
		return true;
	if (!Settings::ProfileExists(name)) {
		if (!create || !Settings::CheckProfileName(name).IsEmpty())
			return false;
		_NewProfile(name);
		return fSettings.profile == name;
	}
	bool wasPowered = fPowered;
	_ClosePreferences();
	PowerOff();
	status_t status = fSettings.SwitchProfile(name);
	_ApplySettings();
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile \"%s\" could not be loaded: %s",
			name, strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return false;
	}
	if (wasPowered)
		PowerOn();
	return true;
}


// A copy of the current profile, which becomes the current one. A running
// machine goes on with the copy's flash and NVRAM.
void
MachineWindow::_NewProfile(const char* name)
{
	_ClosePreferences();
	fSettings.Save();
	status_t status = fSettings.CopyProfile(name);
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile \"%s\" could not be created: %s",
			name, strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return;
	}
	if (fPowered) {
		std::string nvram = fSettings.NvramPath().String();
		std::string pram = fSettings.PramPath().String();
		fEmulator.Post([nvram, pram](powermac::Machine& machine) {
			std::string error;
			machine.SavePersistentState(error);		// to the old profile
			machine.SetPersistentPaths(nvram, pram);
			machine.SavePersistentState(error);
		});
	}
	_ApplySettings();
}


void
MachineWindow::_RenameProfile(const char* name)
{
	_ClosePreferences();
	status_t status = fSettings.RenameProfile(name);
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile could not be renamed: %s",
			strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
	}
	_ApplySettings();
}


// Deletes the current profile and switches to the first of the others.
void
MachineWindow::_DeleteProfile()
{
	BStringList names;
	Settings::GetProfiles(names);
	BString old = fSettings.profile;
	BString next;
	for (int32 i = 0; i < names.CountStrings() && next.IsEmpty(); i++) {
		if (names.StringAt(i) != old)
			next = names.StringAt(i);
	}
	if (next.IsEmpty())
		return;
	_ClosePreferences();
	fSettings.SwitchProfile(next.String());
	status_t status = Settings::DeleteProfile(old.String());
	_ApplySettings();
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile \"%s\" could not be deleted: %s",
			old.String(), strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
	}
}


// After a profile change: the settings that take effect at once.
void
MachineWindow::_ApplySettings()
{
	fEmulator.SetSpeedLimit(fSettings.speedLimit);
	fAudio.SetVolume(fSettings.soundVolume / 100.0f);
	_BuildProfileMenu();
	_UpdateMenus();
	_UpdateStatus();
}


void
MachineWindow::_ClosePreferences()
{
	if (fPrefsWindow.IsValid())
		fPrefsWindow.SendMessage(B_QUIT_REQUESTED);
	fPrefsWindow = BMessenger();
}


BMenu*
MachineWindow::_BuildMediaMenu()
{
	BMenu* menu = new BMenu("Media");
	fInsertCdItem = new BMenuItem("Insert CD" B_UTF8_ELLIPSIS,
		new BMessage(kMsgInsertCd));
	menu->AddItem(fInsertCdItem);
	fReinsertCdItem = new BMenuItem("Reinsert CD",
		new BMessage(kMsgReinsertCd));
	menu->AddItem(fReinsertCdItem);
	fEjectCdItem = new BMenuItem("Eject CD", new BMessage(kMsgEjectCd));
	menu->AddItem(fEjectCdItem);
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Insert floppy" B_UTF8_ELLIPSIS,
		new BMessage(kMsgInsertFloppy)));
	fEjectFloppyItem = new BMenuItem("Eject floppy",
		new BMessage(kMsgEjectFloppy));
	menu->AddItem(fEjectFloppyItem);
	return menu;
}


BMenu*
MachineWindow::_BuildViewMenu()
{
	BMenu* menu = new BMenu("View");
	static const char* const kScaleNames[3] = { "Fit window", "Actual size",
		"Double size" };
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kMsgScale);
		message->AddInt32("scale", i);
		fScaleItems[i] = new BMenuItem(kScaleNames[i], message);
		menu->AddItem(fScaleItems[i]);
	}
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Save screenshot",
		new BMessage(kMsgScreenshot)));
	return menu;
}


bool
MachineWindow::PowerOn()
{
	if (fPowered)
		return true;
	std::string error;
	MachineConfig config = fSettings.MachineConfigForRun();
	_PrepareSharedFolders(config);
	// a floppy the drive does not take leaves it empty (the machine starts)
	if (!config.floppyImage.empty()) {
		std::string floppyError;
		std::unique_ptr<powermac::DiskImage> image = powermac::DiskImage::Open(
			config.floppyImage, true, floppyError);
		if (!image || !powermac::FloppyImage::Wrap(std::move(image),
				floppyError)) {
			BString text;
			text.SetToFormat("The floppy image could not be inserted: %s",
				floppyError.c_str());
			(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
			config.floppyImage.clear();
			fSettings.machine.floppyImage.clear();
			fSettings.Save();
		}
	}
	ScreenView* screen = fScreen;
	// serial ports
	fSerialBackends.clear();
	powermac::SerialBackend* serial[2] = { nullptr, nullptr };
	for (int i = 0; i < 2; i++) {
		BString destination = fSettings.serial[i];
		if (destination == "console") {
			serial[i] = fConsoles[i];
		} else if (!destination.IsEmpty()) {
			std::string serialError;
			std::unique_ptr<powermac::SerialBackend> backend
				= powermac::CreateSerialBackend(destination.String(),
					serialError);
			if (!backend) {
				BString text;
				text.SetToFormat("The %s port is not connected: %s",
					i == 0 ? "modem" : "printer", serialError.c_str());
				(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
				continue;
			}
			serial[i] = backend.get();
			fSerialBackends.push_back(std::move(backend));
		}
	}
	// Ethernet: the user mode network (NAT)
	fNetwork.reset();
	if (fSettings.ethernet) {
		powermac::UserNetworkConfig networkConfig;
		std::string networkError;
		if (powermac::ParsePortForwards(fSettings.portForwards.String(),
				networkConfig.forwards, networkError))
			fNetwork = powermac::CreateUserNetwork(networkConfig, networkError);
		if (!fNetwork) {
			BString text;
			text.SetToFormat("Ethernet is not connected: %s",
				networkError.c_str());
			(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		}
	}
	powermac::NetworkBackend* network = fNetwork.get();
	HaikuAudio* audio = nullptr;
	if (fSettings.soundEnabled) {
		fAudio.SetVolume(fSettings.soundVolume / 100.0f);
		if (fAudio.Start() == B_OK)
			audio = &fAudio;
	}
	if (!fEmulator.Start(config, error,
			[screen, audio, serial, network](powermac::Machine& machine) {
				machine.SetVideoSink(screen);
				machine.SetAudioSink(audio);
				if (network != nullptr)
					machine.SetNetworkBackend(network);
				for (int i = 0; i < 2; i++) {
					if (serial[i] != nullptr)
						machine.SetSerialBackend(i, serial[i]);
				}
			})) {
		BString text("The machine could not be started:\n\n");
		text << error.c_str() << "\n\nCheck the ROM files and the disk "
			"images in the preferences.";
		BAlert* alert = new BAlert(kAppName, text.String(), "OK");
		alert->Go(nullptr);
		return false;
	}
	fEmulator.SetSpeedLimit(fSettings.speedLimit);
	fPowered = true;
	fSystemOn = false;
	fScreen->SetPowered(true);
	_UpdateMenus();
	return true;
}


void
MachineWindow::PowerOff()
{
	if (!fPowered)
		return;
	fEmulator.Stop();
	fScreen->SetPowered(false);
	fAudio.Stop();
	fSerialBackends.clear();
	fNetwork.reset();
	_SyncSharedFolders(true);
	fPowered = false;
	_ReleaseAllKeys();
	_UpdateMenus();
	_UpdateStatus();
}


void
MachineWindow::_UpdateMenus()
{
	fPowerItem->SetLabel(fPowered ? "Power off" : "Power on");
	fPowerItem->Message()->what = fPowered ? kMsgPowerOff : kMsgPowerOn;
	fPowerKeyItem->SetEnabled(fPowered);
	fPauseItem->SetMarked(fEmulator.IsPaused());
	for (int32 i = 0; i < 3; i++)
		fScaleItems[i]->SetMarked(fSettings.scale == (i == 0 ? 0 : i));
	MachineConfig::ScsiTarget* cd = _CdTarget();
	fInsertCdItem->SetEnabled(cd != nullptr);
	fEjectCdItem->SetEnabled(cd != nullptr && !cd->image.empty());
	// the disc the system or the user ejected, back into the drive (the
	// BeOS Launcher ejects the CD it boots BeOS from)
	BString reinsert("Reinsert CD");
	if (!fLastCd.IsEmpty())
		reinsert.SetToFormat("Reinsert \"%s\"", BPath(fLastCd).Leaf());
	fReinsertCdItem->SetLabel(reinsert.String());
	fReinsertCdItem->SetEnabled(cd != nullptr && cd->image.empty()
		&& !fLastCd.IsEmpty());
	fEjectFloppyItem->SetEnabled(!fSettings.machine.floppyImage.empty());
}


void
MachineWindow::_UpdateStatus()
{
	BString text;
	if (!fPowered) {
		text.SetToFormat("%s  \xe2\x80\x94  powered off",
			fSettings.profile.String());
	} else {
		powermac::EmulatorThread::Status status = fEmulator.GetStatus();
		text.SetToFormat("%s  \xe2\x80\x94  Power Macintosh %s, %u MB  "
			"\xe2\x80\x94  %.0f%%, %.0f MIPS", fSettings.profile.String(),
			MachineConfig::Info(fSettings.machine.model).name,
			(unsigned)fSettings.machine.ramMB, status.speedPercent,
			status.mips);
		if (status.paused)
			text << "  (paused)";
		else if (!status.poweredOn)
			text << "  (shut down)";
		static const char* const kHostKeyNames[4] = { "right Ctrl",
			"right Alt", "right Windows key", "Menu key" };
		const char* hostKey = kHostKeyNames[_HostKeyIndex()];
		if (fScreen->IsMouseCaptured())
			text << "  \xe2\x80\x94  " << hostKey << " releases the mouse";
		else if (fSettings.captureOnClick)
			text << "  \xe2\x80\x94  click the screen to capture the mouse";
		else
			text << "  \xe2\x80\x94  " << hostKey << " captures the mouse";
	}
	if (text != fStatus->Text())
		fStatus->SetText(text.String());

	// media icons: what is inserted, the disk activity
	const MachineConfig& machine = fSettings.machine;
	bool present[MediaStatusView::kCount] = { false, false, false };
	BString description;
	present[MediaStatusView::kFloppy] = !machine.floppyImage.empty();
	description << "Floppy: " << (machine.floppyImage.empty()
		? "empty (click to insert)"
		: BPath(machine.floppyImage.c_str()).Leaf());
	const MachineConfig::ScsiTarget* buses[2] = { machine.scsiInternal,
		machine.scsiExternal };
	for (int bus = 0; bus < 2; bus++) {
		for (int id = 0; id < 7; id++) {
			const MachineConfig::ScsiTarget& target = buses[bus][id];
			if (target.kind == MachineConfig::ScsiTarget::kNone)
				continue;
			bool cd = target.kind == MachineConfig::ScsiTarget::kCdRom;
			present[cd ? MediaStatusView::kCd : MediaStatusView::kDisk]
				|= !target.image.empty();
			description << "\n" << (bus == 0 ? "Internal" : "External")
				<< " SCSI "
				<< id << (cd ? " CD-ROM: " : " disk: ")
				<< (target.image.empty() ? (cd ? "empty (click to insert)"
						: "none")
					: BPath(target.image.c_str()).Leaf());
		}
	}
	bool diskActive = false;
	if (fPowered)
		diskActive = fEmulator.GetStatus().disk > 0.05f;
	fMedia->SetState(present, diskActive, description);
}


void
MachineWindow::_FitWindow()
{
	if (fSettings.scale == 0)
		return;
	InvalidateLayout();
	ResizeToPreferred();
}


// #pragma mark - keyboard


void
MachineWindow::_SendKey(uint32 haikuKey, bool down)
{
	if (haikuKey >= 128)
		return;
	int code = powermac::HaikuKeyToAdb(haikuKey);
	if (code < 0)
		return;
	if (!down && !fKeysDown[haikuKey])
		return;
	fKeysDown[haikuKey] = down;
	_SendAdbKey(code, down);
}


void
MachineWindow::_SendAdbKey(int code, bool down)
{
	fEmulator.Post([code, down](powermac::Machine& machine) {
		if (down)
			machine.KeyDown((powermac::u8)code);
		else
			machine.KeyUp((powermac::u8)code);
	});
}


void
MachineWindow::_ReleaseAllKeys()
{
	for (uint32 key = 0; key < 128; key++) {
		if (fKeysDown[key])
			_SendKey(key, false);
	}
}


void
MachineWindow::_ModifiersChanged(uint32 old, uint32 now)
{
	struct Modifier {
		uint32 mask;
		uint32 key;
	};
	static const Modifier kModifiers[] = {
		{ B_LEFT_SHIFT_KEY, 0x4b }, { B_RIGHT_SHIFT_KEY, 0x56 },
		{ B_LEFT_CONTROL_KEY, 0x5c }, { B_RIGHT_CONTROL_KEY, 0x60 },
		{ B_LEFT_COMMAND_KEY, 0x5d }, { B_RIGHT_COMMAND_KEY, 0x5f },
		{ B_LEFT_OPTION_KEY, 0x66 }, { B_RIGHT_OPTION_KEY, 0x67 }
	};
	uint32 hostModifier = kHostKeys[_HostKeyIndex()].modifier;
	for (const Modifier& modifier : kModifiers) {
		if (modifier.mask == hostModifier)
			continue;
		bool was = (old & modifier.mask) != 0;
		bool is = (now & modifier.mask) != 0;
		if (was != is)
			_SendKey(modifier.key, is);
	}
	// Caps Lock locks down on Apple's keyboards: down while locked.
	if (((old ^ now) & B_CAPS_LOCK) != 0)
		_SendAdbKey(powermac::KeyNameToAdb("CapsLock"),
			(now & B_CAPS_LOCK) != 0);
	// Num Lock is Clear (a key press for every change)
	if (((old ^ now) & B_NUM_LOCK) != 0) {
		_SendAdbKey(powermac::KeyNameToAdb("Clear"), true);
		_SendAdbKey(powermac::KeyNameToAdb("Clear"), false);
	}
	// A modifier host key toggles the mouse capture when released.
	if (hostModifier != 0 && (old & hostModifier) != 0
		&& (now & hostModifier) == 0)
		fScreen->SetMouseCaptured(!fScreen->IsMouseCaptured());
}


int32
MachineWindow::_HostKeyIndex() const
{
	return fSettings.hostKey >= 0 && fSettings.hostKey < 4
		? fSettings.hostKey : kHostKeyRightControl;
}


bool
MachineWindow::_HandleKey(BMessage* message)
{
	switch (message->what) {
		case B_KEY_DOWN:
		case B_UNMAPPED_KEY_DOWN:
		{
			int32 key;
			if (message->FindInt32("key", &key) != B_OK)
				return false;
			if (key == (int32)kHostKeys[_HostKeyIndex()].key)
				return true;
			// the lock keys come as modifier changes; ADB keyboards do
			// not repeat (Mac OS does)
			if (key == 0x3b || key == 0x22 || (key < 128 && fKeysDown[key]))
				return true;
			_SendKey(key, true);
			return true;
		}
		case B_KEY_UP:
		case B_UNMAPPED_KEY_UP:
		{
			int32 key;
			if (message->FindInt32("key", &key) != B_OK)
				return false;
			if (key == (int32)kHostKeys[_HostKeyIndex()].key) {
				if (kHostKeys[_HostKeyIndex()].modifier == 0)
					fScreen->SetMouseCaptured(!fScreen->IsMouseCaptured());
				return true;
			}
			if (key == 0x3b || key == 0x22)
				return true;
			_SendKey(key, false);
			return true;
		}
		case B_MODIFIERS_CHANGED:
		{
			int32 now = 0;
			int32 old = 0;
			message->FindInt32("modifiers", &now);
			message->FindInt32("be:old_modifiers", &old);
			_ModifiersChanged(old, now);
			fModifiers = now;
			return true;
		}
	}
	return false;
}


void
MachineWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
	if (fPowered && IsActive()) {
		switch (message->what) {
			case B_KEY_DOWN:
			case B_KEY_UP:
			case B_UNMAPPED_KEY_DOWN:
			case B_UNMAPPED_KEY_UP:
			case B_MODIFIERS_CHANGED:
				if (_HandleKey(message))
					return;
				break;
		}
	}
	BWindow::DispatchMessage(message, handler);
}


void
MachineWindow::WindowActivated(bool active)
{
	if (!active) {
		fScreen->SetMouseCaptured(false);
		_ReleaseAllKeys();
	}
	BWindow::WindowActivated(active);
}


// Restarts the machine with keys held through the start ("C",
// "Command+Option+P+R").
bool
MachineWindow::_RestartHolding(const char* keys, double seconds)
{
	std::vector<powermac::u8> codes;
	BString names(keys);
	int32 start = 0;
	while (start <= names.Length()) {
		int32 plus = names.FindFirst('+', start);
		if (plus < 0)
			plus = names.Length();
		BString name;
		names.CopyInto(name, start, plus - start);
		int code = powermac::KeyNameToAdb(name.Trim().String());
		if (code < 0)
			return false;
		codes.push_back((powermac::u8)code);
		start = plus + 1;
	}
	if (!fPowered && !PowerOn())
		return false;
	_ReleaseAllKeys();
	fEmulator.Post([codes, seconds](powermac::Machine& machine) {
		machine.PowerOn();
		machine.HoldKeys(codes, (powermac::u64)(seconds
			* machine.TicksPerSecond()));
	});
	return true;
}


BString
MachineWindow::_SaveScreenshot(const char* target)
{
	BBitmap* bitmap = fScreen->CopyFrame();
	if (bitmap == nullptr || bitmap->InitCheck() != B_OK) {
		delete bitmap;
		return "";
	}
	BPath path;
	if (target != nullptr)
		path.SetTo(target);
	else {
		if (find_directory(B_DESKTOP_DIRECTORY, &path) != B_OK)
			find_directory(B_USER_DIRECTORY, &path);
		BString name;
		for (int i = 1; i < 1000; i++) {
			name.SetToFormat("PowerMac screenshot %d.png", i);
			BPath candidate(path.Path(), name.String());
			BEntry entry(candidate.Path());
			if (!entry.Exists()) {
				path = candidate;
				break;
			}
		}
	}
	BBitmapStream stream(bitmap);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &file,
		B_PNG_FORMAT);
	// the stream deletes the bitmap unless it is detached
	BBitmap* detached = nullptr;
	if (stream.DetachBitmap(&detached) == B_OK)
		delete detached;
	return path.Path();
}


// #pragma mark - messages


void
MachineWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgTick:
		{
			_UpdateStatus();
			// the system shut down: with shared folders the machine powers
			// off, which brings their changes back (the power key starts it
			// again)
			if (fPowered) {
				bool on = fEmulator.GetStatus().poweredOn;
				if (fSystemOn && !on && !fSharedFolders.empty()) {
					PowerOff();
					break;
				}
				fSystemOn = on;
			}
			// what the system did with the CD (it ejects too)
			int bus, id;
			if (fPowered && _CdDrive(bus, id)) {
				BMessenger window(this);
				fEmulator.Post([window, bus, id](powermac::Machine& machine) {
					BMessage state(kMsgCdState);
					state.AddString("path", machine.CdPath(bus, id).c_str());
					// never blocks: the window thread may be waiting for
					// this one (stopping the machine)
					window.SendMessage(&state, (BHandler*)nullptr, 0);
				});
			}
			if (fPowered) {
				BMessenger window(this);
				fEmulator.Post([window](powermac::Machine& machine) {
					BMessage state(kMsgFloppyState);
					state.AddString("path", machine.FloppyPath().c_str());
					window.SendMessage(&state, (BHandler*)nullptr, 0);
				});
			}
			break;
		}
		case kMsgFloppyState:
		{
			// the system ejects floppies itself
			const char* path = message->GetString("path", "");
			if (fPowered && path[0] == '\0'
				&& !fSettings.machine.floppyImage.empty()) {
				fSettings.machine.floppyImage.clear();
				fSettings.Save();
				_UpdateMenus();
			}
			break;
		}
		case kMsgCdState:
		{
			MachineConfig::ScsiTarget* cd = _CdTarget();
			const char* path = message->GetString("path", "");
			if (cd != nullptr && fPowered && cd->image != path) {
				if (path[0] == '\0' && !cd->image.empty())
					fLastCd = cd->image.c_str();
				cd->image = path;
				_UpdateMenus();
			}
			break;
		}
		case kMsgFrame:
			if (message->GetBool("resized", false))
				_FitWindow();
			break;
		case kMsgPowerOn:
			PowerOn();
			break;
		case kMsgPowerOff:
			PowerOff();
			break;
		case kMsgReset:
			if (!fPowered) {
				PowerOn();
				break;
			}
			_ReleaseAllKeys();
			fEmulator.Post([](powermac::Machine& machine) {
				machine.PowerOn();
			});
			break;
		case kMsgTypeKeys:
		{
			const char* text;
			if (message->FindString("text", &text) == B_OK && fPowered)
				_TypeText(text);
			break;
		}
		case B_GET_PROPERTY:
		case B_SET_PROPERTY:
			if (!_HandleScripting(message))
				BWindow::MessageReceived(message);
			return;
		case kMsgInterruptSwitch:
			fEmulator.Post([](powermac::Machine& machine) {
				machine.PressInterruptSwitch();
			});
			break;
		case kMsgPowerKey:
		{
			// the Mac OS shut down dialog; the machine is off: power on
			if (!fPowered) {
				PowerOn();
				break;
			}
			int code = powermac::KeyNameToAdb("Power");
			fEmulator.Post([code](powermac::Machine& machine) {
				machine.HoldKeys({ (powermac::u8)code },
					machine.MicrosecondsToTicks(200000));
			});
			break;
		}
		case kMsgRestartHolding:
		{
			const char* keys = message->GetString("keys", "");
			double seconds = message->GetDouble("seconds", 10);
			if (!_RestartHolding(keys, seconds)) {
				if (message->IsSourceWaiting()) {
					BMessage reply(B_MESSAGE_NOT_UNDERSTOOD);
					reply.AddInt32("error", B_BAD_VALUE);
					reply.AddString("message", "unknown key or no power");
					message->SendReply(&reply);
				}
				return;
			}
			break;
		}
		case kMsgPause:
			fEmulator.SetPaused(!fEmulator.IsPaused());
			_UpdateMenus();
			break;
		case kMsgScale:
			fSettings.scale = message->GetInt32("scale", 1);
			fScreen->SetScale(fSettings.scale);
			_UpdateMenus();
			_FitWindow();
			break;
		case kMsgScreenshot:
		{
			const char* path = message->GetString("path", nullptr);
			BString saved = _SaveScreenshot(path);
			if (message->IsSourceWaiting()) {
				BMessage reply(B_REPLY);
				reply.AddString("path", saved);
				message->SendReply(&reply);
			}
			return;
		}
		case kMsgInsertCd:
			if (_CdTarget() == nullptr) {
				(new BAlert(kAppName, "There is no CD-ROM drive: add one in "
					"the preferences (Storage).", "OK"))->Go(nullptr);
				break;
			}
			if (fCdPanel == nullptr) {
				fCdPanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), nullptr, B_FILE_NODE, false,
					new BMessage(kMsgCdChosen));
			}
			fCdPanel->Show();
			break;
		case kMsgCdChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) != B_OK)
				break;
			BPath path(&ref);
			_InsertCd(path.Path());
			break;
		}
		case kMsgInsertFloppy:
			if (fFloppyPanel == nullptr) {
				fFloppyPanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), nullptr, B_FILE_NODE, false,
					new BMessage(kMsgFloppyChosen));
			}
			fFloppyPanel->Show();
			break;
		case kMsgFloppyChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) != B_OK)
				break;
			BPath path(&ref);
			_InsertFloppy(path.Path());
			break;
		}
		case kMsgEjectFloppy:
			_EjectFloppy();
			break;
		case kMsgEjectCd:
			_EjectCd();
			break;
		case kMsgReinsertCd:
			if (!fLastCd.IsEmpty())
				_InsertCd(fLastCd.String());
			break;
		case kMsgPreferences:
		{
			BLooper* looper;
			BWindow* window = fPrefsWindow.IsValid()
				? dynamic_cast<BWindow*>(fPrefsWindow.Target(&looper))
				: nullptr;
			if (window != nullptr && window->Lock()) {
				window->Activate();
				window->Unlock();
				break;
			}
			window = new PrefsWindow(fSettings, BMessenger(this));
			fPrefsWindow = BMessenger(window);
			window->Show();
			break;
		}
		case kMsgSwitchProfile:
		{
			const char* name;
			if (message->FindString("name", &name) != B_OK
				|| fSettings.profile == name) {
				_BuildProfileMenu();
				break;
			}
			if (!fPowered || message->GetInt32("which", -1) == 1) {
				SwitchProfile(name, false);
				break;
			}
			if (message->HasInt32("which")) {
				// cancelled
				_BuildProfileMenu();
				break;
			}
			BString text;
			text.SetToFormat("Restart the Mac with the profile \"%s\"?\n\n"
				"The system is not shut down first: if it has unsaved work, "
				"shut it down (Special > Shut Down) before switching.", name);
			BAlert* alert = new BAlert(kAppName, text.String(), "Cancel",
				"Restart", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			BMessage* confirmed = new BMessage(kMsgSwitchProfile);
			confirmed->AddString("name", name);
			alert->Go(new BInvoker(confirmed, this));
			_BuildProfileMenu();
			break;
		}
		case kMsgNewProfile:
		{
			const char* name;
			if (message->FindString("name", &name) == B_OK) {
				_NewProfile(name);
				break;
			}
			BString explanation;
			explanation.SetToFormat("The new profile starts as a copy of "
				"\"%s\".", fSettings.profile.String());
			BString suggestion(fSettings.profile);
			suggestion << " copy";
			for (int i = 2; Settings::ProfileExists(suggestion.String()); i++)
				suggestion.SetToFormat("%s copy %d", fSettings.profile.String(),
					i);
			(new ProfileNameWindow("New profile", explanation.String(),
				suggestion.String(), nullptr, "Create",
				BMessenger(this), BMessage(kMsgNewProfile)))->Show();
			break;
		}
		case kMsgRenameProfile:
		{
			const char* name;
			if (message->FindString("name", &name) == B_OK) {
				_RenameProfile(name);
				break;
			}
			if (fPowered) {
				(new BAlert(kAppName, "Power the Mac off to rename its "
					"profile.", "OK"))->Go(nullptr);
				break;
			}
			BString explanation;
			explanation.SetToFormat("Rename the profile \"%s\":",
				fSettings.profile.String());
			(new ProfileNameWindow("Rename profile", explanation.String(),
				fSettings.profile.String(), fSettings.profile.String(),
				"Rename", BMessenger(this), BMessage(kMsgRenameProfile)))
				->Show();
			break;
		}
		case kMsgDeleteProfile:
		{
			if (message->GetInt32("which", -1) == 1) {
				_DeleteProfile();
				break;
			}
			if (message->HasInt32("which"))
				break;
			if (fPowered) {
				(new BAlert(kAppName, "Power the Mac off to delete its "
					"profile.", "OK"))->Go(nullptr);
				break;
			}
			BString text;
			text.SetToFormat("Delete the profile \"%s\"?\n\nIts settings, "
				"NVRAM and PRAM are deleted; disk images are kept.",
				fSettings.profile.String());
			BAlert* alert = new BAlert(kAppName, text.String(), "Cancel",
				"Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			alert->Go(new BInvoker(new BMessage(kMsgDeleteProfile), this));
			break;
		}
		case kMsgSerialConsole:
			_ShowConsole(message->GetInt32("port", 0));
			break;
		case kMsgResetNvram:
		case kMsgResetPram:
		{
			bool wasPowered = fPowered;
			PowerOff();
			BString path = message->what == kMsgResetNvram
				? fSettings.NvramPath() : fSettings.PramPath();
			unlink(path.String());
			if (wasPowered)
				PowerOn();
			break;
		}
		case kMsgPreferencesChanged:
		{
			fSettings.Save();
			fEmulator.SetSpeedLimit(fSettings.speedLimit);
			fAudio.SetVolume(fSettings.soundVolume / 100.0f);
			fScreen->SetMouseOptions(fSettings.mouseSpeed,
				fSettings.captureOnClick);
			bool restart = message->GetBool("restart", false) && fPowered;
			if (restart)
				PowerOff();
			if (message->GetBool("reset_nvram", false))
				unlink(fSettings.NvramPath().String());
			if (message->GetBool("reset_pram", false))
				unlink(fSettings.PramPath().String());
			if (restart)
				PowerOn();
			else if (fPowered) {
				// a CD changed in the preferences: into the drive
				int bus, id;
				MachineConfig::ScsiTarget* cd = _CdTarget();
				if (cd != nullptr && _CdDrive(bus, id)) {
					BString image(cd->image.c_str());
					if (image.IsEmpty())
						_EjectCd();
					else
						_InsertCd(image.String());
				}
				// and a floppy
				BString floppy(fSettings.machine.floppyImage.c_str());
				if (floppy.IsEmpty())
					_EjectFloppy();
				else
					_InsertFloppy(floppy.String());
			}
			_UpdateMenus();
			break;
		}
		case B_ABOUT_REQUESTED:
			be_app->PostMessage(B_ABOUT_REQUESTED);
			break;
		case kMsgCaptureChanged:
			_UpdateStatus();
			break;
		default:
			BWindow::MessageReceived(message);
			return;
	}
	// scripted commands (hey) get their reply after the action
	if (message->IsSourceWaiting()) {
		BMessage reply(B_REPLY);
		reply.AddInt32("error", B_OK);
		message->SendReply(&reply);
	}
}


// #pragma mark - scripting


static property_info kProperties[] = {
	{ "Running", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"Whether the machine is on; set it to power on or off.", 0,
		{ B_BOOL_TYPE } },
	{ "Speed", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Emulated speed in percent of real time.", 0, { B_DOUBLE_TYPE } },
	{ "Mips", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Million instructions per second (both processors).", 0,
		{ B_DOUBLE_TYPE } },
	{ "EmulatedTime", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Seconds of emulated time since power on.", 0, { B_DOUBLE_TYPE } },
	{ "DiskActivity", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Whether the disks are busy.", 0, { B_BOOL_TYPE } },
	{ "SystemOn", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"False once the system has shut the machine down.", 0,
		{ B_BOOL_TYPE } },
	{ "Pointer", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The guest's pointer (screen pixels; -1 when unknown).", 0,
		{ B_POINT_TYPE } },
	{ "SerialLog", { B_GET_PROPERTY, 0 }, { B_INDEX_SPECIFIER, 0 },
		"Output of serial port 1 (modem) or 2 (printer), when connected to "
		"a console.", 0, { B_STRING_TYPE } },
	{ "Cd", { B_GET_PROPERTY, B_SET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The CD image in the CD-ROM drive; set \"\" to eject.", 0,
		{ B_STRING_TYPE } },
	{ "Floppy", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"The floppy disk image; set \"\" to eject.", 0, { B_STRING_TYPE } },
	{ "AudioStats", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Sound output counters (frames received, dropped, consumed, "
		"output, underruns, buffered; output rate, buffer size and rate "
		"correction).", 0,
		{ B_STRING_TYPE } },
	{ "Profile", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"The machine profile; set to switch (a running machine restarts).",
		0, { B_STRING_TYPE } },
	{ "Profiles", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The names of the machine profiles.", 0, { B_STRING_TYPE } },
	{ 0 }
};


BHandler*
MachineWindow::ResolveSpecifier(BMessage* message, int32 index,
	BMessage* specifier, int32 what, const char* property)
{
	BPropertyInfo info(kProperties);
	if (info.FindMatch(message, index, specifier, what, property) >= 0)
		return this;
	return BWindow::ResolveSpecifier(message, index, specifier, what,
		property);
}


status_t
MachineWindow::GetSupportedSuites(BMessage* data)
{
	data->AddString("suites", "suite/vnd.PowerMac-machine");
	BPropertyInfo info(kProperties);
	data->AddFlat("messages", &info);
	return BWindow::GetSupportedSuites(data);
}


bool
MachineWindow::_HandleScripting(BMessage* message)
{
	int32 index;
	BMessage specifier;
	int32 form;
	const char* property;
	if (message->GetCurrentSpecifier(&index, &specifier, &form, &property)
			!= B_OK)
		return false;
	bool set = message->what == B_SET_PROPERTY;
	powermac::EmulatorThread::Status status = fEmulator.GetStatus();
	BMessage reply(B_REPLY);
	status_t result = B_OK;
	if (strcmp(property, "Running") == 0) {
		if (set) {
			bool on;
			if (message->FindBool("data", &on) != B_OK)
				result = B_BAD_VALUE;
			else if (on && !fPowered)
				result = PowerOn() ? B_OK : B_ERROR;
			else if (!on && fPowered)
				PowerOff();
		} else
			reply.AddBool("result", fPowered);
	} else if (strcmp(property, "Speed") == 0 && !set)
		reply.AddDouble("result", status.speedPercent);
	else if (strcmp(property, "Mips") == 0 && !set)
		reply.AddDouble("result", status.mips);
	else if (strcmp(property, "EmulatedTime") == 0 && !set)
		reply.AddDouble("result", status.emulatedSeconds);
	else if (strcmp(property, "DiskActivity") == 0 && !set)
		reply.AddBool("result", status.disk > 0.05f);
	else if (strcmp(property, "SystemOn") == 0 && !set)
		reply.AddBool("result", fPowered && status.poweredOn);
	else if (strcmp(property, "Pointer") == 0 && !set)
		reply.AddPoint("result", BPoint(status.cursorX, status.cursorY));
	else if (strcmp(property, "SerialLog") == 0 && !set) {
		int32 port = specifier.GetInt32("index", 0);
		if (port < 1 || port > 2 || fConsoles[port - 1] == nullptr)
			result = B_BAD_INDEX;
		else {
			std::string text;
			fConsoles[port - 1]->OutputSince(0, text);
			// a string ends at a NUL (Open Firmware sends one)
			text.erase(std::remove(text.begin(), text.end(), '\0'),
				text.end());
			reply.AddString("result", text.c_str());
		}
	} else if (strcmp(property, "Profile") == 0) {
		if (set) {
			const char* name;
			if (message->FindString("data", &name) != B_OK)
				result = B_BAD_VALUE;
			else if (!Settings::ProfileExists(name))
				result = B_ENTRY_NOT_FOUND;
			else if (!SwitchProfile(name, false))
				result = B_ERROR;
		} else
			reply.AddString("result", fSettings.profile);
	} else if (strcmp(property, "Profiles") == 0 && !set) {
		BStringList names;
		Settings::GetProfiles(names);
		for (int32 i = 0; i < names.CountStrings(); i++)
			reply.AddString("result", names.StringAt(i));
	} else if (strcmp(property, "AudioStats") == 0 && !set) {
		HaikuAudio::Stats stats = fAudio.GetStats();
		BString text;
		text.SetToFormat("received %" B_PRIu64 " dropped %" B_PRIu64
			" consumed %" B_PRIu64 " output %" B_PRIu64 " underruns %"
			B_PRIu32 " buffered %" B_PRIu32 " rate %.0f buffer %" B_PRIu32
			" correction %+.3f%%", stats.received, stats.dropped,
			stats.consumed, stats.output, stats.underruns, stats.buffered,
			stats.outputRate, stats.bufferFrames, stats.correction * 100);
		reply.AddString("result", text);
	} else if (strcmp(property, "Floppy") == 0) {
		if (set) {
			const char* path;
			if (message->FindString("data", &path) != B_OK)
				result = B_BAD_VALUE;
			else if (path[0] == '\0')
				_EjectFloppy();
			else
				_InsertFloppy(path);
		} else
			reply.AddString("result", fSettings.machine.floppyImage.c_str());
	} else if (strcmp(property, "Cd") == 0) {
		MachineConfig::ScsiTarget* cd = _CdTarget();
		if (cd == nullptr)
			result = B_DEV_NO_MEDIA;
		else if (set) {
			const char* path;
			if (message->FindString("data", &path) != B_OK)
				result = B_BAD_VALUE;
			else if (path[0] == '\0')
				_EjectCd();
			else
				_InsertCd(path);
		} else
			reply.AddString("result", cd->image.c_str());
	} else
		return false;
	if (result != B_OK) {
		reply.what = B_MESSAGE_NOT_UNDERSTOOD;
		reply.AddString("message", strerror(result));
	}
	reply.AddInt32("error", result);
	message->SendReply(&reply);
	return true;
}


bool
MachineWindow::_CdDrive(int& bus, int& id) const
{
	const MachineConfig& machine = fSettings.machine;
	// the external bus first: the 9500's own drive is ID 3 there
	const MachineConfig::ScsiTarget* buses[2] = { machine.scsiExternal,
		machine.scsiInternal };
	for (int i = 0; i < 2; i++) {
		for (int target = 0; target < 7; target++) {
			if (buses[i][target].kind == MachineConfig::ScsiTarget::kCdRom) {
				bus = i == 0 ? 1 : 0;
				id = target;
				return true;
			}
		}
	}
	return false;
}


MachineConfig::ScsiTarget*
MachineWindow::_CdTarget()
{
	int bus, id;
	if (!_CdDrive(bus, id))
		return nullptr;
	return bus == 0 ? &fSettings.machine.scsiInternal[id]
		: &fSettings.machine.scsiExternal[id];
}


void
MachineWindow::_InsertCd(const char* path)
{
	int bus, id;
	MachineConfig::ScsiTarget* target = _CdTarget();
	if (target == nullptr || !_CdDrive(bus, id))
		return;
	target->image = path;
	if (fPowered) {
		std::string file = path;
		BMessenger window(this);
		fEmulator.Post([file, bus, id, window](powermac::Machine& machine) {
			if (machine.CdPath(bus, id) == file)
				return;		// in the drive already
			std::string error;
			if (!machine.InsertCd(bus, id, file, error)) {
				BString text;
				text.SetToFormat("The CD image could not be inserted: %s",
					error.c_str());
				(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
			}
		});
	}
	fSettings.Save();
	_UpdateMenus();
}


void
MachineWindow::_EjectCd()
{
	int bus, id;
	MachineConfig::ScsiTarget* target = _CdTarget();
	if (target == nullptr || !_CdDrive(bus, id))
		return;
	if (!target->image.empty())
		fLastCd = target->image.c_str();
	target->image.clear();
	fEmulator.Post([bus, id](powermac::Machine& machine) {
		machine.EjectCd(bus, id);
	});
	fSettings.Save();
	_UpdateMenus();
}


void
MachineWindow::_InsertFloppy(const char* path)
{
	fSettings.machine.floppyImage = path;
	if (fPowered) {
		std::string file = path;
		bool writeProtected = fSettings.machine.floppyWriteProtected;
		fEmulator.Post([file, writeProtected](powermac::Machine& machine) {
			if (machine.FloppyPath() == file)
				return;		// in the drive already
			std::string error;
			if (!machine.InsertFloppy(file, writeProtected, error)) {
				BString text;
				text.SetToFormat("The floppy image could not be inserted: "
					"%s", error.c_str());
				(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
			}
		});
	}
	fSettings.Save();
	_UpdateMenus();
}


void
MachineWindow::_EjectFloppy()
{
	fSettings.machine.floppyImage.clear();
	fEmulator.Post([](powermac::Machine& machine) {
		machine.EjectFloppy();
	});
	fSettings.Save();
	_UpdateMenus();
}


// Shared folders: a volume per folder (BFS for BeOS, HFS for Mac OS),
// built at power on in the profile's folder; the system's changes go back
// to the folder at power off.
void
MachineWindow::_PrepareSharedFolders(MachineConfig& config)
{
	fSharedFolders.clear();
	BString messages;
	MachineConfig::ScsiTarget* buses[2] = { config.scsiInternal,
		config.scsiExternal };
	// a shared Mac volume takes its SCSI driver from one of the disks
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
			BString image = fSettings.ProfileDirectory();
			image << (mac ? "/shared-mac-" : "/shared-")
				<< (bus == 0 ? "int" : "ext") << id << ".qcow2";
			std::unique_ptr<powermac::SharedVolume> share;
			if (mac) {
				share.reset(new powermac::SharedMacFolder(target.image,
					image.String(), disks, &fHostAttributes));
			} else {
				share.reset(new powermac::SharedFolder(target.image,
					image.String(), &fHostAttributes));
			}
			std::string report;
			std::string error;
			if (!share->Prepare(report, error)) {
				messages << error.c_str() << "\n\n";
				target.kind = MachineConfig::ScsiTarget::kNone;
				continue;
			}
			if (!report.empty())
				messages << report.c_str() << "\n\n";
			target.image = image.String();
			target.readOnly = false;
			fSharedFolders.push_back(std::move(share));
		}
	}
	if (messages.Length() > 0) {
		messages.Trim();
		(new BAlert(kAppName, messages.String(), "OK"))->Go(nullptr);
	}
}


void
MachineWindow::_SyncSharedFolders(bool interactive)
{
	BString messages;
	for (auto& share : fSharedFolders) {
		std::string report;
		std::string error;
		if (!share->Sync(report, error))
			messages << error.c_str() << "\n\n";
		else if (!report.empty())
			messages << report.c_str() << "\n\n";
	}
	fSharedFolders.clear();
	if (interactive && messages.Length() > 0) {
		messages.Trim();
		(new BAlert(kAppName, messages.String(), "OK"))->Go(nullptr);
	}
}


// Types ASCII text: each character a key press of 20 ms (with Shift for
// capitals and symbols), 20 ms apart.
void
MachineWindow::_TypeText(const char* text)
{
	std::vector<std::pair<powermac::u8, bool> > events;
	powermac::u8 shift = (powermac::u8)powermac::KeyNameToAdb("Shift");
	for (const char* c = text; *c != '\0'; c++) {
		bool shifted;
		int code = powermac::AsciiToAdb(*c, shifted);
		if (code < 0)
			continue;
		if (shifted)
			events.push_back(std::make_pair(shift, true));
		events.push_back(std::make_pair((powermac::u8)code, true));
		events.push_back(std::make_pair((powermac::u8)code, false));
		if (shifted)
			events.push_back(std::make_pair(shift, false));
	}
	fEmulator.Post([events](powermac::Machine& machine) {
		machine.QueueKeys(events, machine.MicrosecondsToTicks(20000));
	});
}


void
MachineWindow::FrameMoved(BPoint origin)
{
	fSettings.windowFrame = Frame();
	BWindow::FrameMoved(origin);
}


void
MachineWindow::FrameResized(float width, float height)
{
	fSettings.windowFrame = Frame();
	BWindow::FrameResized(width, height);
}


void
MachineWindow::_ShowConsole(int port)
{
	if (port < 0 || port > 1)
		return;
	if (fSettings.serial[port] != "console") {
		BString text;
		text.SetToFormat("The %s port is not set to a console window. "
			"Change it in the preferences (Serial ports).",
			port == 0 ? "modem" : "printer");
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return;
	}
	if (fConsoleWindows[port].IsValid()) {
		BLooper* looper;
		BWindow* window = dynamic_cast<BWindow*>(
			fConsoleWindows[port].Target(&looper));
		if (window != nullptr && window->Lock()) {
			window->Activate();
			window->Unlock();
			return;
		}
	}
	SerialConsoleWindow* window = new SerialConsoleWindow(fConsoles[port]);
	fConsoleWindows[port] = BMessenger(window);
	window->Show();
}


bool
MachineWindow::QuitRequested()
{
	PowerOff();
	fSettings.windowFrame = Frame();
	fSettings.Save();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
