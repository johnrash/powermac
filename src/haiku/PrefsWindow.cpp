/*
 * PowerMac emulator for Haiku - preferences.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "PrefsWindow.h"

#include "PowerMac.h"
#include "NewDiskWindow.h"

#include "core/video/ImsTwinTurbo128.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <CardLayout.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <GridLayout.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MimeType.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>

#include <vector>

#include <string.h>

using powermac::MachineConfig;

enum {
	kMsgOk = 'okay',
	kMsgCancel = 'cncl',
	kMsgBrowse = 'brws',
	kMsgChosen = 'chos',
	kMsgSection = 'sect',
	kMsgNewDisk = 'nwdk',
	kMsgNewDiskInPanel = 'nwdp',
	kMsgSerialKind = 'srkd',
	kMsgResetNvramRequest = 'rnrq',
	kMsgResetPramRequest = 'rprq',
	kMsgResetConfirmed = 'rscf'
};

enum {
	kBrowseRom = 0,
	kBrowseCardRom = 1,
	kBrowseScsiInternal0 = 2,	// 2-8
	kBrowseScsiExternal0 = 9,	// 9-15
	kBrowseFloppy = 16
};

// serial destination kinds (prefixes of the settings string)
static const char* const kSerialKinds[] = { "", "console", "tcp-listen:",
	"tcp-connect:", "host:", "file:" };
static const char* const kSerialLabels[] = { "Not connected",
	"Console window", "TCP server", "TCP client", "Host serial port",
	"Log to file" };
static const char* const kSerialHints[] = { "", "",
	"Port, or address:port (e.g. 2001)", "Host:port (e.g. localhost:2001)",
	"Device (e.g. /dev/ports/usb0)", "File path" };


// A sidebar entry: icon and name.
class SectionItem : public BListItem {
public:
	// 'signatures' lists alternatives separated by '|' (the first one
	// installed gives the icon); "rsrc:NAME" is a vector icon among the
	// application's resources.
	SectionItem(const char* name, const char* signatures)
		:
		fName(name),
		fIcon(nullptr)
	{
		float size = be_control_look->ComposeIconSize(24).Height() + 1;
		BString list(signatures);
		int32 start = 0;
		while (fIcon == nullptr && start < list.Length()) {
			int32 end = list.FindFirst('|', start);
			if (end < 0)
				end = list.Length();
			BString signature;
			list.CopyInto(signature, start, end - start);
			start = end + 1;
			if (signature.StartsWith("rsrc:")) {
				size_t length = 0;
				const void* data = BApplication::AppResources()->LoadResource(
					'VICN', signature.String() + 5, &length);
				if (data == nullptr)
					continue;
				fIcon = new BBitmap(BRect(0, 0, size - 1, size - 1),
					B_RGBA32);
				if (BIconUtils::GetVectorIcon((const uint8*)data, length,
						fIcon) != B_OK) {
					delete fIcon;
					fIcon = nullptr;
				}
				continue;
			}
			BMimeType type(signature.String());
			uint8* data = nullptr;
			size_t length = 0;
			if (type.GetIcon(&data, &length) != B_OK)
				continue;
			fIcon = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
			if (BIconUtils::GetVectorIcon(data, length, fIcon) != B_OK) {
				delete fIcon;
				fIcon = nullptr;
			}
			delete[] data;
		}
	}

	float PreferredWidth() const
	{
		float iconSize = be_control_look->ComposeIconSize(24).Height() + 1;
		return be_plain_font->StringWidth(fName.String()) + iconSize
			+ 3 * be_control_look->DefaultLabelSpacing();
	}

	~SectionItem()
	{
		delete fIcon;
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: ui_color(B_LIST_BACKGROUND_COLOR);
		owner->SetHighColor(background);
		owner->FillRect(frame);
		float iconSize = be_control_look->ComposeIconSize(24).Height() + 1;
		float x = frame.left + be_control_look->DefaultLabelSpacing();
		if (fIcon != nullptr) {
			owner->SetDrawingMode(B_OP_ALPHA);
			owner->DrawBitmap(fIcon, BPoint(x,
				frame.top + (frame.Height() - iconSize) / 2));
			owner->SetDrawingMode(B_OP_COPY);
		}
		x += iconSize + be_control_look->DefaultLabelSpacing();
		font_height height;
		owner->GetFontHeight(&height);
		owner->SetHighColor(IsSelected()
			? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: ui_color(B_LIST_ITEM_TEXT_COLOR));
		owner->SetLowColor(background);
		owner->DrawString(fName.String(), BPoint(x, frame.top
			+ (frame.Height() + height.ascent - height.descent) / 2));
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		float iconSize = be_control_look->ComposeIconSize(24).Height() + 1;
		SetHeight(std::max(Height(), iconSize) + 8);
		SetWidth(Width() + iconSize + 3 * be_control_look->DefaultLabelSpacing());
	}

private:
	BString fName;
	BBitmap* fIcon;
};


PrefsWindow::PrefsWindow(Settings& settings, BMessenger target)
	:
	BWindow(BRect(100, 100, 760, 560), "PowerMac preferences",
		B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fSettings(settings),
	fTarget(target),
	fPanel(nullptr),
	fBrowsing(-1)
{
	fSections = new BListView("sections");
	fSections->SetSelectionMessage(new BMessage(kMsgSection));
	BScrollView* sectionScroll = new BScrollView("scroll", fSections, 0,
		false, false);
	BView* pageHolder = new BView("pages", 0);
	fPages = new BCardLayout();
	pageHolder->SetLayout(fPages);

	BString title;
	title.SetToFormat("PowerMac preferences: %s", fSettings.profile.String());
	SetTitle(title.String());

	_AddSection("Machine", "rsrc:machine|application/x-vnd.Haiku-About",
		_MachinePage());
	_AddSection("ROM & NVRAM", "application/x-vnd.Haiku-Devices",
		_RomPage());
	_AddSection("Display", "application/x-vnd.Haiku-Screen", _DisplayPage());
	_AddSection("Storage", "application/x-vnd.Haiku-DriveSetup",
		_StoragePage());
	_AddSection("Sound", "application/x-vnd.Haiku-Media", _SoundPage());
	_AddSection("Keyboard & mouse", "application/x-vnd.Haiku-Input"
		"|application/x-vnd.Haiku-Mouse", _InputPage());
	_AddSection("Network", "application/x-vnd.Haiku-Network", _NetworkPage());
	_AddSection("Serial ports", "application/x-vnd.Haiku-SerialConnect"
		"|application/x-vnd.Haiku-Terminal", _SerialPage());
	_AddSection("Advanced", "application/x-vnd.Haiku-Debugger",
		_AdvancedPage());

	BButton* ok = new BButton("OK", new BMessage(kMsgOk));
	BButton* cancel = new BButton("Cancel", new BMessage(kMsgCancel));
	BString profileNote;
	profileNote.SetToFormat("The profile \"%s\"; Keyboard & mouse are the "
		"same in all profiles.", fSettings.profile.String());
	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(sectionScroll, 0)
			.Add(pageHolder, 1)
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.AddGroup(B_VERTICAL, 0)
				.Add(new BStringView("profile", profileNote.String()))
				.Add(new BStringView("note", "Changes to the hardware "
					"restart the machine."))
			.End()
			.AddGlue()
			.Add(cancel)
			.Add(ok)
		.End();
	float width = 0;
	for (int32 i = 0; i < fSections->CountItems(); i++) {
		width = std::max(width,
			((SectionItem*)fSections->ItemAt(i))->PreferredWidth());
	}
	sectionScroll->SetExplicitMinSize(BSize(width + 16, 300));
	sectionScroll->SetExplicitMaxSize(BSize(width + 16, B_SIZE_UNLIMITED));
	fSections->Select(0);
	ok->MakeDefault(true);
	_UpdateSerialHints();
	CenterOnScreen();
}


PrefsWindow::~PrefsWindow()
{
	delete fPanel;
}


void
PrefsWindow::_AddSection(const char* name, const char* iconSignature,
	BView* page)
{
	fSections->AddItem(new SectionItem(name, iconSignature));
	fPages->AddView(page);
}


// #pragma mark - pages


BView*
PrefsWindow::_MachinePage()
{
	const MachineConfig& machine = fSettings.machine;
	static const char* const kModels[] = {
		"9500/120 (PowerPC 604, 120 MHz)",
		"9500/132 (PowerPC 604, 132 MHz)",
		"9500/150 (PowerPC 604e, 150 MHz)",
		"9500/180MP (2 \xc3\x97 PowerPC 604e, 180 MHz)",
		"9500/200 (PowerPC 604e, 200 MHz)" };
	static const int32 kModelValues[] = { MachineConfig::k9500_120,
		MachineConfig::k9500_132, MachineConfig::k9500_150,
		MachineConfig::k9500_180MP, MachineConfig::k9500_200 };
	BMenuField* model = _PopUp("Model:", kModels, kModelValues, 5,
		machine.model, &fModel);
	static const char* const kMemory[] = { "16 MB", "32 MB", "48 MB",
		"64 MB", "96 MB", "128 MB", "192 MB", "256 MB", "384 MB", "512 MB",
		"768 MB", "1 GB", "1.5 GB" };
	static const int32 kMemoryValues[] = { 16, 32, 48, 64, 96, 128, 192, 256,
		384, 512, 768, 1024, 1536 };
	BMenuField* memory = _PopUp("Memory:", kMemory, kMemoryValues, 13,
		machine.ramMB, &fMemory);
	static const char* const kCpus[] = { "As the model has",
		"One (the second one removed)" };
	static const int32 kCpuValues[] = { 0, 1 };
	BMenuField* cpus = _PopUp("Processors:", kCpus, kCpuValues, 2,
		machine.cpuCount, &fCpus);
	static const char* const kSpeeds[] = { "50%", "Real time", "200%",
		"As fast as possible" };
	static const int32 kSpeedValues[] = { 50, 100, 200, 0 };
	BMenuField* speed = _PopUp("Speed:", kSpeeds, kSpeedValues, 4,
		fSettings.speedLimit, &fSpeed);

	BView* page = new BView("machine", 0);
	BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddMenuField(model, 0, 0)
		.AddMenuField(memory, 0, 1)
		.AddMenuField(cpus, 0, 2)
		.AddMenuField(speed, 0, 3)
		.Add(new BStringView("mp", "The 9500/180MP has a second processor "
			"card; Mac OS uses it with MP-aware"), 0, 4, 2)
		.Add(new BStringView("mp2", "software, BeOS for everything."), 0, 5,
			2)
		.AddGlue(0, 6, 2);
	return page;
}


BView*
PrefsWindow::_RomPage()
{
	fRom = new BTextControl("ROM:", fSettings.romPath.String(), nullptr);
	fRom->SetExplicitMinSize(BSize(280, B_SIZE_UNSET));
	BMessage* browse = new BMessage(kMsgBrowse);
	browse->AddInt32("which", kBrowseRom);
	BButton* chooseRom = new BButton("Choose" B_UTF8_ELLIPSIS, browse);
	fCardRom = new BTextControl("Video card ROM:",
		fSettings.cardRomPath.String(), nullptr);
	browse = new BMessage(kMsgBrowse);
	browse->AddInt32("which", kBrowseCardRom);
	BButton* chooseCardRom = new BButton("Choose" B_UTF8_ELLIPSIS, browse);
	fClock = new BTextControl("Clock:", fSettings.clock.String(), nullptr);
	fClock->SetToolTip("Empty: this computer's time. Otherwise the date and "
		"time the Mac starts at, \"YYYY-MM-DD HH:MM\" (BeOS DR8 expires).");
	BButton* resetNvram = new BButton("Reset NVRAM" B_UTF8_ELLIPSIS,
		new BMessage(kMsgResetNvramRequest));
	BButton* resetPram = new BButton("Reset PRAM" B_UTF8_ELLIPSIS,
		new BMessage(kMsgResetPramRequest));

	BView* page = new BView("rom", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddGrid()
			.AddTextControl(fRom, 0, 0)
			.Add(chooseRom, 2, 0)
			.AddTextControl(fCardRom, 0, 1)
			.Add(chooseCardRom, 2, 1)
			.AddTextControl(fClock, 0, 2)
		.End()
		.Add(new BStringView("rom1", "The ROM: the 4 MB ROM of a Power "
			"Macintosh 7200, 7500, 8500 or 9500"))
		.Add(new BStringView("rom2", "($077D.28F2 or $077D.28F1)."))
		.Add(new BStringView("card1", "The video card ROM: the 64 KB ROM of "
			"an IMS Twin Turbo 128 card, which carries"))
		.Add(new BStringView("card2", "its Mac OS driver. Without it there "
			"is no display."))
		.Add(new BStringView("clock", "Clock: empty follows this computer; "
			"\"1997-03-01 12:00\" starts there every time."))
		.AddStrut(B_USE_DEFAULT_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(resetNvram)
			.Add(resetPram)
			.AddGlue()
		.End()
		.Add(new BStringView("nvram", "The NVRAM holds Open Firmware's "
			"settings; the PRAM Mac OS's (startup disk, ...)."))
		.AddGlue();
	return page;
}


BView*
PrefsWindow::_DisplayPage()
{
	const MachineConfig& machine = fSettings.machine;
	static const char* const kVram[] = { "2 MB", "4 MB", "8 MB" };
	static const int32 kVramValues[] = { 2, 4, 8 };
	BMenuField* vram = _PopUp("Video memory:", kVram, kVramValues, 3,
		machine.vramMB, &fVram);
	int count = powermac::ImsTwinTurbo128::MonitorCount();
	std::vector<const char*> monitors;
	std::vector<int32> monitorValues;
	for (int i = 0; i < count; i++) {
		monitors.push_back(powermac::ImsTwinTurbo128::kMonitors[i].name);
		monitorValues.push_back(i);
	}
	BMenuField* monitor = _PopUp("Monitor:", monitors.data(),
		monitorValues.data(), count, machine.monitor, &fMonitor);

	BView* page = new BView("display", 0);
	BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddMenuField(vram, 0, 0)
		.AddMenuField(monitor, 0, 1)
		.Add(new BStringView("card", "The card is an IMS Twin Turbo 128 "
			"(slot A1). The monitor decides"), 0, 2, 2)
		.Add(new BStringView("card2", "the resolutions Mac OS offers "
			"(Monitors control panel)."), 0, 3, 2)
		.AddGlue(0, 4, 2);
	return page;
}


BView*
PrefsWindow::_StoragePage()
{
	const MachineConfig& machine = fSettings.machine;
	BView* page = new BView("storage", 0);
	BGridLayout* grid = BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.SetColumnWeight(2, 1.0f);

	static const char* const kKinds[] = { "None", "Disk", "CD-ROM",
		"Shared Mac", "Shared Be" };
	static const int32 kKindValues[] = { MachineConfig::ScsiTarget::kNone,
		MachineConfig::ScsiTarget::kDisk, MachineConfig::ScsiTarget::kCdRom,
		MachineConfig::ScsiTarget::kSharedMac,
		MachineConfig::ScsiTarget::kSharedBe };
	static const char* const kBusNames[2] = { "Internal SCSI bus (hard "
		"disks):", "External SCSI bus (the CD-ROM drive is ID 3):" };
	int row = 0;
	for (int bus = 0; bus < 2; bus++) {
		const MachineConfig::ScsiTarget* targets = bus == 0
			? machine.scsiInternal : machine.scsiExternal;
		BStringView* title = new BStringView("bus", kBusNames[bus]);
		title->SetFont(be_bold_font);
		grid->AddView(title, 0, row++, 5);
		for (int id = 0; id < 7; id++, row++) {
			const MachineConfig::ScsiTarget& target = targets[id];
			BString label;
			label.SetToFormat("ID %d:", id);
			BMenuField* kind = _PopUp(nullptr, kKinds, kKindValues, 5,
				target.kind, &fScsiKind[bus][id]);
			fScsi[bus][id] = new BTextControl(nullptr, target.image.c_str(),
				nullptr);
			fScsi[bus][id]->SetExplicitMinSize(BSize(240, B_SIZE_UNSET));
			fScsiReadOnly[bus][id] = new BCheckBox("Read only");
			fScsiReadOnly[bus][id]->SetValue(target.readOnly);
			BMessage* browse = new BMessage(kMsgBrowse);
			browse->AddInt32("which", (bus == 0 ? kBrowseScsiInternal0
				: kBrowseScsiExternal0) + id);
			grid->AddView(new BStringView("label", label.String()), 0, row);
			grid->AddView(kind, 1, row);
			grid->AddView(fScsi[bus][id], 2, row);
			grid->AddView(new BButton("Choose" B_UTF8_ELLIPSIS, browse), 3,
				row);
			grid->AddView(fScsiReadOnly[bus][id], 4, row);
		}
	}
	BStringView* floppyTitle = new BStringView("floppy",
		"Floppy drive (400 KB to 1.44 MB images, raw or Disk Copy 4.2):");
	floppyTitle->SetFont(be_bold_font);
	grid->AddView(floppyTitle, 0, row++, 5);
	fFloppy = new BTextControl(nullptr, machine.floppyImage.c_str(), nullptr);
	fFloppyProtected = new BCheckBox("Write protect");
	fFloppyProtected->SetValue(machine.floppyWriteProtected);
	BMessage* browseFloppy = new BMessage(kMsgBrowse);
	browseFloppy->AddInt32("which", kBrowseFloppy);
	grid->AddView(new BStringView("label", "Disk:"), 0, row);
	grid->AddView(fFloppy, 2, row);
	grid->AddView(new BButton("Choose" B_UTF8_ELLIPSIS, browseFloppy), 3, row);
	grid->AddView(fFloppyProtected, 4, row);
	row++;
	BButton* newDisk = new BButton("New disk image" B_UTF8_ELLIPSIS,
		new BMessage(kMsgNewDisk));
	grid->AddView(newDisk, 2, row + 1);
	grid->AddView(new BStringView("hint", "Disk images: raw, or QCOW2 "
		"(grows as it is used). CD-ROMs: ISO, BIN/CUE."), 0, row + 2, 5);
	grid->AddView(new BStringView("hint2", "A new disk is initialized with "
		"Drive Setup from the Mac OS CD."), 0, row + 3, 5);
	grid->AddView(new BStringView("share", "Shared Mac: a Haiku folder Mac "
		"OS sees as a disk (with the driver of a Drive Setup disk)."), 0,
		row + 4, 5);
	grid->AddView(new BStringView("share2", "Shared Be: one BeOS sees. "
		"Changes come back to the folder after the system shut down."), 0,
		row + 5, 5);
	grid->AddItem(BSpaceLayoutItem::CreateGlue(), 0, row + 6, 5);
	return page;
}


BView*
PrefsWindow::_SoundPage()
{
	fSound = new BCheckBox("Play the sound output (AWACS)");
	fSound->SetValue(fSettings.soundEnabled);
	fVolume = new BSlider("volume", "Volume", nullptr, 0, 100,
		B_HORIZONTAL);
	fVolume->SetValue(fSettings.soundVolume);
	fVolume->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fVolume->SetHashMarkCount(11);
	fVolume->SetLimitLabels("Mute", "Full");

	BView* page = new BView("sound", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.Add(fSound)
		.Add(fVolume)
		.AddGlue();
	return page;
}


BView*
PrefsWindow::_InputPage()
{
	static const char* const kKeys[] = { "Right Control", "Right Alt",
		"Right Windows key", "Menu key" };
	static const int32 kKeyValues[] = { kHostKeyRightControl,
		kHostKeyRightCommand, kHostKeyRightOption, kHostKeyMenu };
	BMenuField* hostKey = _PopUp("Mouse release key:", kKeys, kKeyValues, 4,
		fSettings.hostKey, &fHostKey);
	fMouseSpeed = new BSlider("mousespeed", "Mouse speed", nullptr, 25, 400,
		B_HORIZONTAL);
	fMouseSpeed->SetValue(fSettings.mouseSpeed);
	fMouseSpeed->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fMouseSpeed->SetHashMarkCount(8);
	fMouseSpeed->SetLimitLabels("Slow", "Fast");
	fCaptureOnClick = new BCheckBox("Clicking the screen captures the mouse");
	fCaptureOnClick->SetValue(fSettings.captureOnClick);

	BView* page = new BView("input", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddGrid()
			.AddMenuField(hostKey, 0, 0)
		.End()
		.Add(new BStringView("keys", "While the window is active, all other "
			"keys go to the Mac's keyboard:"))
		.Add(new BStringView("keys2", "Alt is Command, the Windows key "
			"Option."))
		.Add(new BStringView("release", "The release key also captures the "
			"mouse again."))
		.AddStrut(B_USE_DEFAULT_SPACING)
		.Add(fMouseSpeed)
		.Add(fCaptureOnClick)
		.AddGlue();
	return page;
}


// The built-in Ethernet (MACE) on a private network (NAT) through the
// host.
BView*
PrefsWindow::_NetworkPage()
{
	fEthernet = new BCheckBox("Ethernet (built in, MACE)");
	fEthernet->SetValue(fSettings.ethernet);
	fMacAddress = new BTextControl("MAC address:",
		fSettings.macAddress.String(), nullptr);
	fPortForwards = new BTextControl("Forwarded host ports:",
		fSettings.portForwards.String(), nullptr);
	fPortForwards->SetToolTip("Host ports that reach the Mac, e.g. "
		"\"tcp:2121:21, tcp:8080:80\" (host port:Mac port)");

	BView* page = new BView("network", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.Add(fEthernet)
		.AddGrid()
			.AddTextControl(fMacAddress, 0, 0)
			.AddTextControl(fPortForwards, 0, 1)
		.End()
		.AddStrut(B_USE_DEFAULT_SPACING)
		.Add(new BStringView("net1", "The Mac is on a private network behind "
			"the host (NAT): use DHCP"))
		.Add(new BStringView("net2", "(Mac OS: TCP/IP control panel), or IP "
			"10.0.2.15, mask 255.255.255.0,"))
		.Add(new BStringView("net3", "router 10.0.2.2 and DNS 10.0.2.3. The "
			"host is 10.0.2.2."))
		.AddGlue();
	return page;
}


BView*
PrefsWindow::_SerialPage()
{
	BView* page = new BView("serial", 0);
	BGridLayout* grid = BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.SetColumnWeight(2, 1.0f);
	static const char* const kNames[] = { "Modem port:", "Printer port:" };
	int32 values[6] = { 0, 1, 2, 3, 4, 5 };
	for (int i = 0; i < 2; i++) {
		BString destination = fSettings.serial[i];
		int32 current = 0;
		BString parameter;
		for (int k = 5; k > 0; k--) {
			if (destination.StartsWith(kSerialKinds[k])) {
				current = k;
				parameter = destination.String() + strlen(kSerialKinds[k]);
				break;
			}
		}
		BMenuField* kind = _PopUp(nullptr, kSerialLabels, values, 6,
			current, &fSerialKind[i], kMsgSerialKind);
		fSerial[i] = new BTextControl(nullptr, parameter.String(), nullptr);
		fSerial[i]->SetExplicitMinSize(BSize(240, B_SIZE_UNSET));
		grid->AddView(new BStringView("label", kNames[i]), 0, i);
		grid->AddView(kind, 1, i);
		grid->AddView(fSerial[i], 2, i);
	}
	grid->AddView(new BStringView("hint", "The modem port is Open "
		"Firmware's console (Machine \xe2\x96\xb8 Restart holding"), 0, 2, 3);
	grid->AddView(new BStringView("hint2", "\xe2\x96\xb8 Command-Option-O-F "
		"shows its prompt there)."), 0, 3, 3);
	grid->AddItem(BSpaceLayoutItem::CreateGlue(), 0, 4, 3);
	return page;
}


BView*
PrefsWindow::_AdvancedPage()
{
	const MachineConfig& machine = fSettings.machine;
	static const char* const kQuanta[] = { "1 \xc2\xb5s", "5 \xc2\xb5s",
		"10 \xc2\xb5s (default)", "50 \xc2\xb5s" };
	static const int32 kQuantumValues[] = { 1, 5, 10, 50 };
	BMenuField* quantum = _PopUp("CPU time slice:", kQuanta, kQuantumValues,
		4, machine.quantumUs, &fQuantum);
	static const char* const kCpis[] = { "0.5", "1.0 (default)", "1.5",
		"2.0" };
	static const int32 kCpiValues[] = { 50, 100, 150, 200 };
	BMenuField* cpi = _PopUp("Cycles per instruction:", kCpis, kCpiValues,
		4, machine.cpiPercent, &fCpi);
	fThreads = new BCheckBox("Run each processor on a host core of its own");
	fThreads->SetValue(machine.threadPerCpu);

	BView* page = new BView("advanced", 0);
	BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddMenuField(quantum, 0, 0)
		.AddMenuField(cpi, 0, 1)
		.Add(fThreads, 1, 2)
		.Add(new BStringView("hint", "The processors run in slices of "
			"emulated time; shorter slices are more exact."), 0, 3, 2)
		.Add(new BStringView("hint2", "On cores of their own, both run at "
			"once when both are busy (up to twice as fast)."), 0, 4, 2)
		.AddGlue(0, 5, 2);
	return page;
}


// #pragma mark -


BMenuField*
PrefsWindow::_PopUp(const char* label, const char* const* items,
	const int32* values, int count, int32 current, BPopUpMenu** menu,
	uint32 what)
{
	*menu = new BPopUpMenu("");
	for (int i = 0; i < count; i++) {
		BMessage* message = new BMessage(what);
		message->AddInt32("value", values[i]);
		BMenuItem* item = new BMenuItem(items[i], message);
		(*menu)->AddItem(item);
		if (values[i] == current)
			item->SetMarked(true);
	}
	if ((*menu)->FindMarked() == nullptr)
		(*menu)->ItemAt(0)->SetMarked(true);
	return new BMenuField(label, *menu);
}


int32
PrefsWindow::_Selected(BPopUpMenu* menu) const
{
	BMenuItem* item = menu->FindMarked();
	if (item == nullptr || item->Message() == nullptr)
		return 0;
	return item->Message()->GetInt32("value", 0);
}


bool
PrefsWindow::_ScsiSlot(int32 which, BPopUpMenu*& kind, BTextControl*& image)
{
	if (which >= kBrowseScsiInternal0 && which < kBrowseScsiInternal0 + 7) {
		kind = fScsiKind[0][which - kBrowseScsiInternal0];
		image = fScsi[0][which - kBrowseScsiInternal0];
		return true;
	}
	if (which >= kBrowseScsiExternal0 && which < kBrowseScsiExternal0 + 7) {
		kind = fScsiKind[1][which - kBrowseScsiExternal0];
		image = fScsi[1][which - kBrowseScsiExternal0];
		return true;
	}
	return false;
}


// Disk slots get a "New disk image" button in the panel: the new image
// goes into the slot.
void
PrefsWindow::_Browse(int32 which, bool directory)
{
	delete fPanel;
	BMessage message(kMsgChosen);
	fPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), nullptr,
		directory ? B_DIRECTORY_NODE : B_FILE_NODE, false, &message);
	fBrowsing = which;

	BString title;
	bool disk = false;
	BPopUpMenu* kindMenu;
	BTextControl* image;
	if (_ScsiSlot(which, kindMenu, image)) {
		bool internal = which < kBrowseScsiExternal0;
		title.SetToFormat("%s SCSI ID %d", internal ? "Internal" : "External",
			(int)(which - (internal ? kBrowseScsiInternal0
				: kBrowseScsiExternal0)));
		int32 kind = _Selected(kindMenu);
		disk = kind == MachineConfig::ScsiTarget::kNone
			|| kind == MachineConfig::ScsiTarget::kDisk;
	} else if (which == kBrowseRom)
		title = "ROM";
	else if (which == kBrowseCardRom)
		title = "Video card ROM";
	else if (which == kBrowseFloppy)
		title = "Floppy disk";
	BWindow* window = fPanel->Window();
	if (window->Lock()) {
		window->SetTitle(BString(kAppName).Append(": ").Append(title)
			.String());
		BView* background = window->ChildAt(0);
		BView* cancel = background != nullptr
			? background->FindView("cancel button") : nullptr;
		if (disk && cancel != nullptr) {
			BMessage* create = new BMessage(kMsgNewDiskInPanel);
			create->AddInt32("which", which);
			BButton* button = new BButton(BRect(0, 0, 10, 10), "new disk",
				"New disk image" B_UTF8_ELLIPSIS, create,
				B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);
			button->ResizeToPreferred();
			BRect frame = cancel->Frame();
			button->MoveTo(frame.left - button->Bounds().Width()
				- be_control_look->DefaultItemSpacing(),
				frame.top + (frame.Height() - button->Bounds().Height()) / 2);
			button->SetTarget(this);
			background->AddChild(button);
		}
		window->Unlock();
	}
	fPanel->Show();
}


void
PrefsWindow::_UpdateSerialHints()
{
	for (int i = 0; i < 2; i++) {
		int32 kind = _Selected(fSerialKind[i]);
		fSerial[i]->SetEnabled(kind >= 2);
		fSerial[i]->SetToolTip(kSerialHints[kind]);
	}
}


void
PrefsWindow::_Apply()
{
	MachineConfig& machine = fSettings.machine;
	MachineConfig old = machine;
	machine.model = (MachineConfig::Model)_Selected(fModel);
	machine.ramMB = _Selected(fMemory);
	machine.cpuCount = _Selected(fCpus);
	machine.vramMB = _Selected(fVram);
	machine.monitor = _Selected(fMonitor);
	machine.quantumUs = _Selected(fQuantum);
	machine.cpiPercent = _Selected(fCpi);
	machine.threadPerCpu = fThreads->Value() != 0;
	bool restart = machine.model != old.model || machine.ramMB != old.ramMB
		|| machine.cpuCount != old.cpuCount || machine.vramMB != old.vramMB
		|| machine.monitor != old.monitor
		|| machine.quantumUs != old.quantumUs
		|| machine.cpiPercent != old.cpiPercent
		|| machine.threadPerCpu != old.threadPerCpu;
	for (int bus = 0; bus < 2; bus++) {
		MachineConfig::ScsiTarget* targets = bus == 0 ? machine.scsiInternal
			: machine.scsiExternal;
		const MachineConfig::ScsiTarget* oldTargets = bus == 0
			? old.scsiInternal : old.scsiExternal;
		for (int id = 0; id < 7; id++) {
			MachineConfig::ScsiTarget& target = targets[id];
			target.kind = (MachineConfig::ScsiTarget::Kind)
				_Selected(fScsiKind[bus][id]);
			target.image = fScsi[bus][id]->Text();
			target.readOnly = fScsiReadOnly[bus][id]->Value() != 0
				|| target.kind == MachineConfig::ScsiTarget::kCdRom;
		}
		// a device without an image would stop the machine from starting
		Settings::DropEmptyDevices(machine);
		for (int id = 0; id < 7; id++) {
			// a CD changed in a drive that stays needs no restart
			bool cdSwap = targets[id].kind
					== MachineConfig::ScsiTarget::kCdRom
				&& oldTargets[id].kind == MachineConfig::ScsiTarget::kCdRom;
			restart |= targets[id].kind != oldTargets[id].kind
				|| (!cdSwap && targets[id].image != oldTargets[id].image)
				|| targets[id].readOnly != oldTargets[id].readOnly;
		}
	}
	// a floppy goes into the running machine; protecting it again needs a
	// restart
	BString floppy = fFloppy->Text();
	floppy.Trim();
	machine.floppyImage = floppy.String();
	machine.floppyWriteProtected = fFloppyProtected->Value() != 0;
	restart |= !floppy.IsEmpty()
		&& machine.floppyWriteProtected != old.floppyWriteProtected;
	BString romPath = fRom->Text();
	BString cardRomPath = fCardRom->Text();
	BString clock = fClock->Text();
	clock.Trim();
	powermac::u32 macTime;
	if (!Settings::ParseClock(clock.String(), macTime)) {
		(new BAlert(kAppName, "The clock is neither empty nor a date and time "
			"like \"1997-03-01 12:00\": it follows this computer.",
			"OK"))->Go(nullptr);
		clock = "";
	}
	restart |= romPath != fSettings.romPath
		|| cardRomPath != fSettings.cardRomPath || clock != fSettings.clock;
	fSettings.romPath = romPath;
	fSettings.cardRomPath = cardRomPath;
	fSettings.clock = clock;
	for (int i = 0; i < 2; i++) {
		int32 kind = _Selected(fSerialKind[i]);
		BString destination = kSerialKinds[kind];
		if (kind >= 2)
			destination << fSerial[i]->Text();
		restart |= destination != fSettings.serial[i];
		fSettings.serial[i] = destination;
	}
	bool ethernet = fEthernet->Value() != 0;
	BString macAddress = fMacAddress->Text();
	BString portForwards = fPortForwards->Text();
	restart |= ethernet != fSettings.ethernet
		|| macAddress != fSettings.macAddress
		|| portForwards != fSettings.portForwards;
	fSettings.ethernet = ethernet;
	fSettings.macAddress = macAddress;
	fSettings.portForwards = portForwards;
	fSettings.hostKey = _Selected(fHostKey);
	fSettings.mouseSpeed = fMouseSpeed->Value();
	fSettings.captureOnClick = fCaptureOnClick->Value() != 0;
	bool sound = fSound->Value() != 0;
	restart |= sound != fSettings.soundEnabled;
	fSettings.soundEnabled = sound;
	fSettings.soundVolume = fVolume->Value();
	fSettings.speedLimit = _Selected(fSpeed);

	BMessage message(kMsgPreferencesChanged);
	message.AddBool("restart", restart);
	fTarget.SendMessage(&message);
}


void
PrefsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSection:
		{
			int32 index = fSections->CurrentSelection();
			if (index >= 0)
				fPages->SetVisibleItem(index);
			break;
		}
		case kMsgSerialKind:
			_UpdateSerialHints();
			break;
		case kMsgBrowse:
		{
			int32 which = message->GetInt32("which", 0);
			BPopUpMenu* kind;
			BTextControl* image;
			int32 selected = _ScsiSlot(which, kind, image)
				? _Selected(kind) : MachineConfig::ScsiTarget::kNone;
			bool folder = selected == MachineConfig::ScsiTarget::kSharedMac
				|| selected == MachineConfig::ScsiTarget::kSharedBe;
			_Browse(which, folder);
			break;
		}
		case kMsgChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) != B_OK)
				break;
			BPath path(&ref);
			BPopUpMenu* kindMenu;
			BTextControl* image;
			if (fBrowsing == kBrowseRom)
				fRom->SetText(path.Path());
			else if (fBrowsing == kBrowseCardRom)
				fCardRom->SetText(path.Path());
			else if (fBrowsing == kBrowseFloppy)
				fFloppy->SetText(path.Path());
			else if (_ScsiSlot(fBrowsing, kindMenu, image)) {
				image->SetText(path.Path());
				BEntry entry(&ref);
				int32 kind = _Selected(kindMenu);
				if (entry.IsDirectory()) {
					// a shared folder: for Mac OS unless chosen for BeOS
					if (kind != MachineConfig::ScsiTarget::kSharedBe)
						kindMenu->ItemAt(3)->SetMarked(true);
				} else if (kind == MachineConfig::ScsiTarget::kNone) {
					// guess the kind from the name
					BString name(path.Leaf());
					name.ToLower();
					int guess = name.EndsWith(".iso") || name.EndsWith(".cue")
						|| name.EndsWith(".bin") || name.EndsWith(".toast")
						|| name.EndsWith(".cdr") ? 2 : 1;
					kindMenu->ItemAt(guess)->SetMarked(true);
				}
			}
			break;
		}
		case kMsgNewDisk:
			(new NewDiskWindow(BMessenger(this)))->Show();
			break;
		case kMsgNewDiskInPanel:
		{
			// in the folder the panel shows, for the slot it was opened for
			if (fPanel == nullptr)
				break;
			entry_ref folder;
			fPanel->GetPanelDirectory(&folder);
			(new NewDiskWindow(BMessenger(this),
				message->GetInt32("which", -1), &folder))->Show();
			break;
		}
		case kMsgNewDiskCreated:
		{
			const char* path;
			if (message->FindString("path", &path) != B_OK)
				break;
			int32 which = message->GetInt32("which", -1);
			BPopUpMenu* kindMenu;
			BTextControl* image;
			if (_ScsiSlot(which, kindMenu, image)) {
				kindMenu->ItemAt(1)->SetMarked(true);
				image->SetText(path);
				if (fPanel != nullptr && fBrowsing == which)
					fPanel->Hide();
				break;
			}
			// from the Storage page: into the first free internal slot
			for (int id = 0; id < 7; id++) {
				if (_Selected(fScsiKind[0][id]) == 0) {
					fScsiKind[0][id]->ItemAt(1)->SetMarked(true);
					fScsi[0][id]->SetText(path);
					break;
				}
			}
			break;
		}
		case kMsgResetNvramRequest:
		case kMsgResetPramRequest:
		{
			bool nvram = message->what == kMsgResetNvramRequest;
			BAlert* alert = new BAlert(kAppName, nvram
				? "Reset the NVRAM? Open Firmware's settings are lost. The "
					"machine restarts."
				: "Reset the PRAM? Mac OS's settings kept there (startup "
					"disk, ...) are lost. The machine restarts.",
				"Cancel", nvram ? "Reset NVRAM" : "Reset PRAM", nullptr,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			BMessage* confirmed = new BMessage(kMsgResetConfirmed);
			confirmed->AddBool("nvram", nvram);
			alert->Go(new BInvoker(confirmed, this));
			break;
		}
		case kMsgResetConfirmed:
			if (message->GetInt32("which", 0) == 1) {
				fTarget.SendMessage(message->GetBool("nvram", false)
					? kMsgResetNvram : kMsgResetPram);
			}
			break;
		case kMsgOk:
			_Apply();
			Quit();
			break;
		case kMsgCancel:
			Quit();
			break;
		default:
			BWindow::MessageReceived(message);
			break;
	}
}
