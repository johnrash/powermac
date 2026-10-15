/*
 * PowerMac emulator for Haiku - persistent settings and machine profiles.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "Settings.h"

#include "core/host/MacTime.h"

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using powermac::MachineConfig;

static const char* const kDefaultProfile = "Default";
// The files of a profile copied with it.
static const char* const kProfileFiles[] = { "nvram.bin", "pram.bin" };


static status_t
CopyFile(const char* source, const char* target)
{
	BFile in(source, B_READ_ONLY);
	status_t status = in.InitCheck();
	if (status != B_OK)
		return status;
	BFile out(target, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = out.InitCheck();
	if (status != B_OK)
		return status;
	char buffer[65536];
	ssize_t read;
	while ((read = in.Read(buffer, sizeof(buffer))) > 0) {
		if (out.Write(buffer, read) != read)
			return B_IO_ERROR;
	}
	return read < 0 ? (status_t)read : B_OK;
}


static status_t
ReadArchive(const char* path, BMessage& archive)
{
	BFile file(path, B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	return archive.Unflatten(&file);
}


static status_t
WriteArchive(const char* path, const BMessage& archive)
{
	BFile file(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	return archive.Flatten(&file);
}


Settings::Settings()
	:
	scale(1),
	hostKey(kHostKeyRightControl),
	mouseSpeed(100),
	captureOnClick(true),
	windowFrame(80, 80, 80 + 1040, 80 + 820)
{
	srandom((unsigned)system_time());
	_ResetProfile();
}


BString
Settings::Directory()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path, true) != B_OK)
		path.SetTo("/boot/home/config/settings");
	path.Append("PowerMac");
	create_directory(path.Path(), 0755);
	return BString(path.Path());
}


BString
Settings::ProfilesDirectory()
{
	BString directory = Directory();
	directory << "/Profiles";
	create_directory(directory.String(), 0755);
	return directory;
}


BString
Settings::ProfileDirectory() const
{
	BString directory = ProfilesDirectory();
	directory << "/" << profile;
	return directory;
}


// #pragma mark - profile settings


void
Settings::_ResetProfile()
{
	machine = MachineConfig();
	// each processor on a host core of its own, as in the BeBox emulator
	machine.threadPerCpu = true;
	// the 9500's internal CD-ROM drive: ID 3 on the external bus
	machine.scsiExternal[3].kind = MachineConfig::ScsiTarget::kCdRom;
	romPath = Directory();
	romPath << "/ROMs/9500.rom";
	cardRomPath = Directory();
	cardRomPath << "/ROMs/TwinTurbo128.rom";
	clock = "";
	speedLimit = 100;
	soundEnabled = true;
	soundVolume = 100;
	serial[0] = "console";		// the modem port: Open Firmware's console
	serial[1] = "";
	ethernet = false;
	// a locally made up address after Apple's prefix
	macAddress.SetToFormat("00:05:02:%02x:%02x:%02x", (int)(random() & 0xff),
		(int)(random() & 0xff), (int)(random() & 0xff));
	portForwards = "";
}


static void
ReadTargets(const BMessage& archive, const char* prefix,
	MachineConfig::ScsiTarget* targets)
{
	for (int id = 0; id < 7; id++) {
		MachineConfig::ScsiTarget& target = targets[id];
		BString name;
		int32 value;
		BString string;
		bool flag;
		name.SetToFormat("%s%d_kind", prefix, id);
		if (archive.FindInt32(name.String(), &value) == B_OK)
			target.kind = (MachineConfig::ScsiTarget::Kind)value;
		name.SetToFormat("%s%d_image", prefix, id);
		if (archive.FindString(name.String(), &string) == B_OK)
			target.image = string.String();
		name.SetToFormat("%s%d_read_only", prefix, id);
		if (archive.FindBool(name.String(), &flag) == B_OK)
			target.readOnly = flag;
	}
}


static void
WriteTargets(BMessage& archive, const char* prefix,
	const MachineConfig::ScsiTarget* targets)
{
	for (int id = 0; id < 7; id++) {
		const MachineConfig::ScsiTarget& target = targets[id];
		BString name;
		name.SetToFormat("%s%d_kind", prefix, id);
		archive.AddInt32(name.String(), target.kind);
		name.SetToFormat("%s%d_image", prefix, id);
		archive.AddString(name.String(), target.image.c_str());
		name.SetToFormat("%s%d_read_only", prefix, id);
		archive.AddBool(name.String(), target.readOnly);
	}
}


void
Settings::_ReadProfile(const BMessage& archive)
{
	int32 value;
	bool flag;
	BString string;
	MachineConfig::Model model;
	if (archive.FindString("model", &string) == B_OK
		&& MachineConfig::ModelFromName(string.String(), model))
		machine.model = model;
	if (archive.FindInt32("ram", &value) == B_OK)
		machine.ramMB = value;
	if (archive.FindInt32("cpus", &value) == B_OK)
		machine.cpuCount = value;
	if (archive.FindInt32("vram", &value) == B_OK)
		machine.vramMB = value;
	if (archive.FindInt32("monitor", &value) == B_OK)
		machine.monitor = value;
	if (archive.FindInt32("quantum", &value) == B_OK)
		machine.quantumUs = value;
	if (archive.FindInt32("cpi_percent", &value) == B_OK)
		machine.cpiPercent = value;
	// Up to 0.1.0-4 the profiles saved "thread_per_cpu", off by default: it
	// is ignored, so they run each processor on its own core as new ones do.
	if (archive.FindBool("threads", &flag) == B_OK)
		machine.threadPerCpu = flag;
	if (archive.FindString("rom", &string) == B_OK)
		romPath = string;
	if (archive.FindString("card_rom", &string) == B_OK)
		cardRomPath = string;
	if (archive.FindString("clock", &string) == B_OK)
		clock = string;
	ReadTargets(archive, "scsi_int", machine.scsiInternal);
	ReadTargets(archive, "scsi_ext", machine.scsiExternal);
	if (archive.FindString("floppy", &string) == B_OK)
		machine.floppyImage = string.String();
	if (archive.FindBool("floppy_write_protected", &flag) == B_OK)
		machine.floppyWriteProtected = flag;
	DropEmptyDevices(machine);
	if (archive.FindInt32("speed_limit", &value) == B_OK)
		speedLimit = value;
	if (archive.FindBool("sound", &flag) == B_OK)
		soundEnabled = flag;
	if (archive.FindInt32("sound_volume", &value) == B_OK)
		soundVolume = value;
	for (int i = 0; i < 2; i++) {
		BString name;
		name.SetToFormat("serial%d", i + 1);
		if (archive.FindString(name.String(), &string) == B_OK)
			serial[i] = string;
	}
	if (archive.FindBool("ethernet", &flag) == B_OK)
		ethernet = flag;
	if (archive.FindString("mac_address", &string) == B_OK)
		macAddress = string;
	if (archive.FindString("port_forwards", &string) == B_OK)
		portForwards = string;
}


void
Settings::_WriteProfile(BMessage& archive) const
{
	archive.AddString("model", MachineConfig::Info(machine.model).name);
	archive.AddInt32("ram", machine.ramMB);
	archive.AddInt32("cpus", machine.cpuCount);
	archive.AddInt32("vram", machine.vramMB);
	archive.AddInt32("monitor", machine.monitor);
	archive.AddInt32("quantum", machine.quantumUs);
	archive.AddInt32("cpi_percent", machine.cpiPercent);
	archive.AddBool("threads", machine.threadPerCpu);
	archive.AddString("rom", romPath);
	archive.AddString("card_rom", cardRomPath);
	archive.AddString("clock", clock);
	WriteTargets(archive, "scsi_int", machine.scsiInternal);
	WriteTargets(archive, "scsi_ext", machine.scsiExternal);
	archive.AddString("floppy", machine.floppyImage.c_str());
	archive.AddBool("floppy_write_protected", machine.floppyWriteProtected);
	archive.AddInt32("speed_limit", speedLimit);
	archive.AddBool("sound", soundEnabled);
	archive.AddInt32("sound_volume", soundVolume);
	for (int i = 0; i < 2; i++) {
		BString name;
		name.SetToFormat("serial%d", i + 1);
		archive.AddString(name.String(), serial[i]);
	}
	archive.AddBool("ethernet", ethernet);
	archive.AddString("mac_address", macAddress);
	archive.AddString("port_forwards", portForwards);
}


status_t
Settings::_LoadProfile()
{
	_ResetProfile();
	BMessage archive;
	BString path = ProfileDirectory();
	path << "/settings";
	status_t status = ReadArchive(path.String(), archive);
	if (status == B_OK)
		_ReadProfile(archive);
	return status;
}


status_t
Settings::_SaveProfile() const
{
	BString directory = ProfileDirectory();
	create_directory(directory.String(), 0755);
	BMessage archive('PMxp');
	_WriteProfile(archive);
	directory << "/settings";
	return WriteArchive(directory.String(), archive);
}


// #pragma mark -


status_t
Settings::Load()
{
	BString path = Directory();
	path << "/settings";
	BMessage archive;
	ReadArchive(path.String(), archive);

	int32 value;
	bool flag;
	BString string;
	if (archive.FindInt32("scale", &value) == B_OK)
		scale = value;
	if (archive.FindInt32("host_key", &value) == B_OK)
		hostKey = value;
	if (archive.FindInt32("mouse_speed", &value) == B_OK)
		mouseSpeed = value;
	if (archive.FindBool("capture_on_click", &flag) == B_OK)
		captureOnClick = flag;
	BRect frame;
	if (archive.FindRect("window_frame", &frame) == B_OK)
		windowFrame = frame;

	BStringList profiles;
	GetProfiles(profiles);
	if (profiles.IsEmpty()) {
		_ResetProfile();
		profile = kDefaultProfile;
		return Save();
	}
	if (archive.FindString("profile", &string) == B_OK
		&& ProfileExists(string.String()))
		profile = string;
	else
		profile = profiles.StringAt(0);
	return _LoadProfile();
}


status_t
Settings::Save() const
{
	BMessage archive('PMxs');
	archive.AddString("profile", profile);
	archive.AddInt32("scale", scale);
	archive.AddInt32("host_key", hostKey);
	archive.AddInt32("mouse_speed", mouseSpeed);
	archive.AddBool("capture_on_click", captureOnClick);
	archive.AddRect("window_frame", windowFrame);

	BString path = Directory();
	path << "/settings";
	status_t status = WriteArchive(path.String(), archive);
	if (status != B_OK)
		return status;
	return _SaveProfile();
}


bool
Settings::ParseClock(const char* text, powermac::u32& macTime)
{
	BString trimmed(text);
	trimmed.Trim();
	macTime = 0;
	if (trimmed.IsEmpty())
		return true;
	int year, month, day, hour = 0, minute = 0;
	int fields = sscanf(trimmed.String(), "%d-%d-%d %d:%d", &year, &month,
		&day, &hour, &minute);
	if (fields != 3 && fields != 5)
		return false;
	return powermac::MacTimeFromDate(year, month, day, hour, minute,
		macTime);
}


MachineConfig
Settings::MachineConfigForRun() const
{
	MachineConfig config = machine;
	DropEmptyDevices(config);
	config.romPath = romPath.String();
	config.cardRomPath = cardRomPath.String();
	config.nvramPath = NvramPath().String();
	config.pramPath = PramPath().String();
	ParseClock(clock.String(), config.fixedTime);
	config.ethernet = ethernet;
	unsigned int mac[6];
	if (sscanf(macAddress.String(), "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1],
			&mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
		for (int i = 0; i < 6; i++)
			config.ethernetMac[i] = (powermac::u8)mac[i];
	}
	return config;
}


BString
Settings::NvramPath() const
{
	return ProfileDirectory().Append("/nvram.bin");
}


BString
Settings::PramPath() const
{
	return ProfileDirectory().Append("/pram.bin");
}


void
Settings::DropEmptyDevices(MachineConfig& machine)
{
	MachineConfig::ScsiTarget* buses[2] = { machine.scsiInternal,
		machine.scsiExternal };
	for (MachineConfig::ScsiTarget* targets : buses) {
		for (int id = 0; id < 7; id++) {
			MachineConfig::ScsiTarget& target = targets[id];
			BString image(target.image.c_str());
			if (image.Trim().IsEmpty()) {
				target.image.clear();
				if (target.kind != MachineConfig::ScsiTarget::kCdRom)
					target.kind = MachineConfig::ScsiTarget::kNone;
			}
		}
	}
	BString floppy(machine.floppyImage.c_str());
	if (floppy.Trim().IsEmpty())
		machine.floppyImage.clear();
}


// #pragma mark - profiles


// A profile is a folder in Profiles/ with a settings file (a folder
// without one, say with an NVRAM image put there for a new profile, is not
// a profile yet).
void
Settings::GetProfiles(BStringList& names)
{
	names.MakeEmpty();
	BDirectory directory(ProfilesDirectory().String());
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		if (entry.IsDirectory() && entry.GetName(name) == B_OK
			&& ProfileExists(name))
			names.Add(name);
	}
	names.Sort(true);
}


bool
Settings::ProfileExists(const char* name)
{
	if (name == nullptr || name[0] == '\0')
		return false;
	BString path = ProfilesDirectory();
	path << "/" << name << "/settings";
	return BEntry(path.String()).IsFile();
}


BString
Settings::CheckProfileName(const char* name)
{
	BString trimmed(name);
	trimmed.Trim();
	if (trimmed.IsEmpty())
		return "Enter a name for the profile.";
	if (trimmed != name)
		return "The name cannot begin or end with spaces.";
	if (trimmed.FindFirst('/') >= 0 || trimmed.StartsWith("."))
		return "The name cannot contain \"/\" or begin with a period.";
	if (trimmed.Length() >= B_FILE_NAME_LENGTH)
		return "The name is too long.";
	if (ProfileExists(name))
		return "There is a profile with that name already.";
	BString path = ProfilesDirectory();
	path << "/" << name;
	BEntry entry(path.String());
	if (entry.Exists() && !entry.IsDirectory())
		return "There is a file with that name in the profiles folder.";
	return "";
}


status_t
Settings::SwitchProfile(const char* name)
{
	if (!ProfileExists(name))
		return B_ENTRY_NOT_FOUND;
	status_t status = Save();
	if (status != B_OK)
		return status;
	profile = name;
	_LoadProfile();
	return Save();
}


status_t
Settings::CopyProfile(const char* name)
{
	BString source = ProfileDirectory();
	BString target = ProfilesDirectory();
	target << "/" << name;
	status_t status = create_directory(target.String(), 0755);
	if (status != B_OK)
		return status;
	for (const char* file : kProfileFiles) {
		BString from = source;
		from << "/" << file;
		BString to = target;
		to << "/" << file;
		if (!BEntry(to.String()).Exists() && BEntry(from.String()).Exists()) {
			status = CopyFile(from.String(), to.String());
			if (status != B_OK)
				return status;
		}
	}
	profile = name;
	return Save();
}


status_t
Settings::RenameProfile(const char* name)
{
	BEntry entry(ProfileDirectory().String());
	status_t status = entry.Rename(name);
	if (status != B_OK)
		return status;
	profile = name;
	return Save();
}


status_t
Settings::DeleteProfile(const char* name)
{
	if (!ProfileExists(name))
		return B_ENTRY_NOT_FOUND;
	BString path = ProfilesDirectory();
	path << "/" << name;
	BDirectory directory(path.String());
	BEntry entry;
	std::vector<BEntry> entries;
	while (directory.GetNextEntry(&entry) == B_OK)
		entries.push_back(entry);
	for (BEntry& file : entries) {
		status_t status = file.Remove();
		if (status != B_OK)
			return status;
	}
	return BEntry(path.String()).Remove();
}
