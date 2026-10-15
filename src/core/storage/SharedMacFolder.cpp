/*
 * PowerMac emulator - a host folder shared with Mac OS as a SCSI disk.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "SharedMacFolder.h"

#include "ApplePartitionMap.h"
#include "DiskImage.h"
#include "MacRoman.h"
#include "Qcow2Image.h"
#include "SharedFolderUtil.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>

namespace powermac {

namespace {

const s64 kMacEpochOffset = 2082844800;		// 1904 to 1970
const u16 kIsInvisible = 0x4000;			// Finder flag

const char* const kResourceFork = "MACOS:RFORK";
const char* const kFinderInfo = "MACOS:FINDER_INFO";
const char* const kCreator = "MACOS:CREATOR";
const char* const kFlags = "MACOS:HFS_FLAGS";
const char* const kMimeAttribute = "BEOS:TYPE";

constexpr u32
Code(const char (&c)[5])
{
	return ((u32)(u8)c[0] << 24) | ((u32)(u8)c[1] << 16)
		| ((u32)(u8)c[2] << 8) | (u32)(u8)c[3];
}

// Haiku's attribute types
const u32 kRawType = Code("RAWT");
const u32 kUint16Type = Code("USHT");
const u32 kUint32Type = Code("ULNG");
const u32 kMimeStringType = Code("MIMS");

struct TypeMapping {
	const char*	key;
	u32			type;
	u32			creator;
};

// MIME types and extensions to the Finder's type and creator: SimpleText,
// PictureViewer, MoviePlayer, Acrobat, StuffIt Expander, Netscape, Word.
const TypeMapping kMimeTypes[] = {
	{ "text/plain", Code("TEXT"), Code("ttxt") },
	{ "text/html", Code("TEXT"), Code("MOSS") },
	{ "text/rtf", Code("TEXT"), Code("MSWD") },
	{ "application/pdf", Code("PDF "), Code("CARO") },
	{ "application/x-stuffit", Code("SIT!"), Code("SITx") },
	{ "application/zip", Code("ZIP "), Code("SITx") },
	{ "application/x-gzip", Code("Gzip"), Code("SITx") },
	{ "application/gzip", Code("Gzip"), Code("SITx") },
	{ "application/x-tar", Code("TARF"), Code("SITx") },
	{ "application/x-macbinary", Code("BINA"), Code("SITx") },
	{ "application/mac-binhex40", Code("TEXT"), Code("SITx") },
	{ "image/jpeg", Code("JPEG"), Code("ogle") },
	{ "image/gif", Code("GIFf"), Code("ogle") },
	{ "image/png", Code("PNGf"), Code("ogle") },
	{ "image/tiff", Code("TIFF"), Code("ogle") },
	{ "image/bmp", Code("BMPf"), Code("ogle") },
	{ "image/x-bmp", Code("BMPf"), Code("ogle") },
	{ "image/x-pict", Code("PICT"), Code("ogle") },
	{ "image/x-photoshop", Code("8BPS"), Code("8BIM") },
	{ "audio/x-aiff", Code("AIFF"), Code("TVOD") },
	{ "audio/x-wav", Code("WAVE"), Code("TVOD") },
	{ "audio/x-midi", Code("Midi"), Code("TVOD") },
	{ "audio/mpeg", Code("MPG3"), Code("TVOD") },
	{ "video/quicktime", Code("MooV"), Code("TVOD") },
	{ "video/mpeg", Code("MPEG"), Code("TVOD") },
	{ "video/x-msvideo", Code("VfW "), Code("TVOD") },
};

const TypeMapping kExtensions[] = {
	{ "txt", Code("TEXT"), Code("ttxt") },
	{ "text", Code("TEXT"), Code("ttxt") },
	{ "c", Code("TEXT"), Code("ttxt") },
	{ "h", Code("TEXT"), Code("ttxt") },
	{ "cpp", Code("TEXT"), Code("ttxt") },
	{ "md", Code("TEXT"), Code("ttxt") },
	{ "log", Code("TEXT"), Code("ttxt") },
	{ "csv", Code("TEXT"), Code("ttxt") },
	{ "htm", Code("TEXT"), Code("MOSS") },
	{ "html", Code("TEXT"), Code("MOSS") },
	{ "rtf", Code("TEXT"), Code("MSWD") },
	{ "pdf", Code("PDF "), Code("CARO") },
	{ "sit", Code("SIT!"), Code("SITx") },
	{ "hqx", Code("TEXT"), Code("SITx") },
	{ "bin", Code("BINA"), Code("SITx") },
	{ "zip", Code("ZIP "), Code("SITx") },
	{ "gz", Code("Gzip"), Code("SITx") },
	{ "tgz", Code("Gzip"), Code("SITx") },
	{ "tar", Code("TARF"), Code("SITx") },
	{ "jpg", Code("JPEG"), Code("ogle") },
	{ "jpeg", Code("JPEG"), Code("ogle") },
	{ "gif", Code("GIFf"), Code("ogle") },
	{ "png", Code("PNGf"), Code("ogle") },
	{ "tif", Code("TIFF"), Code("ogle") },
	{ "tiff", Code("TIFF"), Code("ogle") },
	{ "bmp", Code("BMPf"), Code("ogle") },
	{ "pct", Code("PICT"), Code("ogle") },
	{ "pict", Code("PICT"), Code("ogle") },
	{ "psd", Code("8BPS"), Code("8BIM") },
	{ "aif", Code("AIFF"), Code("TVOD") },
	{ "aiff", Code("AIFF"), Code("TVOD") },
	{ "wav", Code("WAVE"), Code("TVOD") },
	{ "mid", Code("Midi"), Code("TVOD") },
	{ "midi", Code("Midi"), Code("TVOD") },
	{ "mp3", Code("MPG3"), Code("TVOD") },
	{ "mov", Code("MooV"), Code("TVOD") },
	{ "mpg", Code("MPEG"), Code("TVOD") },
	{ "mpeg", Code("MPEG"), Code("TVOD") },
	{ "avi", Code("VfW "), Code("TVOD") },
};

// Files new from Mac OS: a MIME type for Haiku
const struct {
	u32			type;
	const char*	mime;
} kTypeMimes[] = {
	{ Code("TEXT"), "text/plain" },
	{ Code("PDF "), "application/pdf" },
	{ Code("SIT!"), "application/x-stuffit" },
	{ Code("ZIP "), "application/zip" },
	{ Code("BINA"), "application/x-macbinary" },
	{ Code("JPEG"), "image/jpeg" },
	{ Code("GIFf"), "image/gif" },
	{ Code("PNGf"), "image/png" },
	{ Code("TIFF"), "image/tiff" },
	{ Code("PICT"), "image/x-pict" },
	{ Code("AIFF"), "audio/x-aiff" },
	{ Code("WAVE"), "audio/x-wav" },
	{ Code("MooV"), "video/quicktime" },
	{ Code("MPEG"), "video/mpeg" },
};

// What the Finder keeps at a volume's root (invisible): not for the host.
// Its Trash and Desktop Folder are kept while they hold something.
const char* const kHousekeeping[] = {
	"Desktop DB", "Desktop DF", "Desktop", "Temporary Items",
	"TheVolumeSettingsFolder", "TheFindByContentFolder",
	"Network Trash Folder", "Shutdown Check"
};


bool
IsHousekeeping(const HfsReader::Entry& entry)
{
	if ((LoadBE16(entry.finderInfo + 8) & kIsInvisible) == 0)
		return false;
	for (const char* name : kHousekeeping) {
		if (entry.name == name)
			return true;
	}
	return false;
}


bool
IsSpecialFolder(const std::string& name)
{
	return name == "Trash" || name == "Desktop Folder";
}


u64
HashInfo(const u8* info)
{
	u64 hash = 14695981039346656037ull;
	for (int i = 0; i < 32; i++)
		hash = (hash ^ info[i]) * 1099511628211ull;
	return hash;
}


// 'name' with 'suffix' before its extension, shortened to 'maxLength'.
std::string
FitName(const std::string& name, const std::string& suffix,
	size_t maxLength = 31)
{
	std::string base = name;
	std::string extension;
	size_t dot = name.rfind('.');
	if (dot != std::string::npos && dot > 0 && name.size() - dot <= 6) {
		base = name.substr(0, dot);
		extension = name.substr(dot);
	}
	size_t room = maxLength - suffix.size() - extension.size();
	if (base.size() > room) {
		base.resize(room);
		while (base.size() > 1 && base.back() == ' ')
			base.pop_back();
	}
	return base + suffix + extension;
}


// A host name for an HFS name: never "." or "..", nor with a NUL.
std::string
SafeHostName(const std::string& macName)
{
	std::string name = MacToHostName(macName);
	std::replace(name.begin(), name.end(), '\0', '_');
	if (name.empty() || name == "." || name == "..")
		name = "_" + name;
	return name;
}


struct HfsNameLess {
	bool operator()(const std::string& a, const std::string& b) const
	{
		return CompareHfsNames(a, b) < 0;
	}
};


const std::vector<u8>*
FindAttribute(const std::vector<BfsAttribute>& attributes, const char* name)
{
	for (const BfsAttribute& attribute : attributes) {
		if (attribute.name == name)
			return &attribute.data;
	}
	return nullptr;
}


s64
BirthTime(const struct stat& st)
{
#ifdef __HAIKU__
	return st.st_crtim.tv_sec;
#else
	return st.st_mtim.tv_sec;
#endif
}

}	// namespace


SharedMacFolder::SharedMacFolder(const std::string& folder,
	const std::string& image, const std::vector<std::string>& driverDisks,
	HostAttributes* attributes)
	:
	fFolder(folder),
	fImage(image),
	fDriverDisks(driverDisks),
	fAttributes(attributes)
{
	while (fFolder.size() > 1 && fFolder.back() == '/')
		fFolder.pop_back();
}


void
SharedMacFolder::TypeAndCreator(const std::string& name,
	const std::string& mimeType, u32& type, u32& creator)
{
	type = 0;
	creator = 0;
	// SheepShaver's Mac types on BeOS
	static const char kMacPrefix[] = "application/x-MacOS-";
	if (mimeType.size() == sizeof(kMacPrefix) - 1 + 4
		&& mimeType.compare(0, sizeof(kMacPrefix) - 1, kMacPrefix) == 0) {
		type = LoadBE32((const u8*)mimeType.data() + sizeof(kMacPrefix) - 1);
		return;
	}
	for (const TypeMapping& mapping : kMimeTypes) {
		if (mimeType == mapping.key) {
			type = mapping.type;
			creator = mapping.creator;
			return;
		}
	}
	if (mimeType.compare(0, 5, "text/") == 0) {
		type = Code("TEXT");
		creator = Code("ttxt");
		return;
	}
	size_t dot = name.rfind('.');
	if (dot == std::string::npos || dot == 0)
		return;
	std::string extension = name.substr(dot + 1);
	for (char& c : extension)
		c = (char)tolower((u8)c);
	for (const TypeMapping& mapping : kExtensions) {
		if (extension == mapping.key) {
			type = mapping.type;
			creator = mapping.creator;
			return;
		}
	}
}


const char*
SharedMacFolder::MimeType(u32 type)
{
	for (const auto& mapping : kTypeMimes) {
		if (mapping.type == type)
			return mapping.mime;
	}
	return "";
}


u32
SharedMacFolder::ToMacTime(s64 seconds)
{
	time_t time = (time_t)seconds;
	struct tm local;
	localtime_r(&time, &local);
	s64 mac = seconds + local.tm_gmtoff + kMacEpochOffset;
	return (u32)std::max<s64>(0, std::min<s64>(mac, 0xffffffffll));
}


s64
SharedMacFolder::FromMacTime(u32 macTime)
{
	time_t time = (time_t)((s64)macTime - kMacEpochOffset);
	struct tm local;
	localtime_r(&time, &local);
	return (s64)time - local.tm_gmtoff;
}


// #pragma mark - building the volume


void
SharedMacFolder::_FinderInfo(const std::string& path, const std::string& name,
	bool directory, bool atRoot, u8 info[32])
{
	memset(info, 0, 32);
	std::vector<BfsAttribute> attributes;
	if (fAttributes != nullptr)
		fAttributes->Read(path, attributes);
	const std::vector<u8>* data = FindAttribute(attributes, kFinderInfo);
	bool stored = data != nullptr && data->size() == 32;
	if (stored)
		memcpy(info, data->data(), 32);
	else if (!directory) {
		std::string mimeType;
		if ((data = FindAttribute(attributes, kMimeAttribute)) != nullptr)
			mimeType.assign(data->begin(), std::find(data->begin(),
				data->end(), 0));
		u32 type;
		u32 creator;
		TypeAndCreator(name, mimeType, type, creator);
		StoreBE32(info, type);
		StoreBE32(info + 4, creator);
	}
	// SheepShaver's, which may be newer
	if (!directory && (data = FindAttribute(attributes, kCreator)) != nullptr
		&& data->size() == 4)
		memcpy(info + 4, data->data(), 4);
	if ((data = FindAttribute(attributes, kFlags)) != nullptr
		&& data->size() == 2)
		memcpy(info + 8, data->data(), 2);
	if (!stored && atRoot && directory && IsSpecialFolder(name))
		StoreBE16(info + 8, LoadBE16(info + 8) | kIsInvisible);
}


bool
SharedMacFolder::_Scan(const std::string& relative, const std::string& hfsPath,
	HfsWriter::Node& node, u64& bytes, std::string& error)
{
	std::string path = fFolder + relative;
	DIR* dir = opendir(path.c_str());
	if (dir == nullptr) {
		error = "cannot read the folder " + path;
		return false;
	}
	std::vector<std::string> names;
	while (struct dirent* entry = readdir(dir)) {
		std::string name = entry->d_name;
		if (name != "." && name != "..")
			names.push_back(name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());

	std::set<std::string, HfsNameLess> taken;
	for (const std::string& name : names) {
		std::string childRelative = relative + "/" + name;
		std::string full = fFolder + childRelative;
		struct stat st;
		if (lstat(full.c_str(), &st) != 0)
			continue;
		if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
			continue;		// symbolic links and special files stay out
		if (S_ISREG(st.st_mode) && (u64)st.st_size > HfsWriter::kMaxForkSize) {
			error = "the file " + full + " is too big for Mac OS (2 GB)";
			return false;
		}

		HfsWriter::Node child;
		std::string macName = HostToMacName(name);
		child.name = FitName(macName, "");
		for (int n = 1; taken.count(child.name) > 0; n++)
			child.name = FitName(macName, "#" + std::to_string(n));
		taken.insert(child.name);
		child.created = ToMacTime(BirthTime(st));
		child.modified = ToMacTime(st.st_mtim.tv_sec);
		_FinderInfo(full, name, S_ISDIR(st.st_mode), relative.empty(),
			child.finderInfo);

		Record record;
		record.hostPath = childRelative;
		record.hostModified = ModifiedNs(st);
		record.macModified = child.modified;
		record.infoHash = HashInfo(child.finderInfo);
		std::string childHfs = hfsPath + ":" + child.name;
		if (S_ISDIR(st.st_mode)) {
			child.directory = true;
			record.directory = true;
			if (!_Scan(childRelative, childHfs, child, bytes, error))
				return false;
		} else {
			child.dataSize = (u64)st.st_size;
			child.readData = [full](u64 offset, void* buffer, size_t length) {
				int fd = open(full.c_str(), O_RDONLY);
				if (fd < 0)
					return false;
				size_t done = 0;
				while (done < length) {
					ssize_t got = pread(fd, (u8*)buffer + done, length - done,
						(off_t)(offset + done));
					if (got <= 0)
						break;
					done += (size_t)got;
				}
				close(fd);
				// a file that shrank meanwhile reads as zeros
				memset((u8*)buffer + done, 0, length - done);
				return true;
			};
			u64 resourceSize = 0;
			if (fAttributes != nullptr
				&& fAttributes->AttributeSize(full, kResourceFork,
					resourceSize)
				&& resourceSize > 0 && resourceSize <= HfsWriter::kMaxForkSize) {
				child.resourceSize = resourceSize;
				// read whole when the writer starts on it
				HostAttributes* attributes = fAttributes;
				std::shared_ptr<std::vector<u8> > fork(new std::vector<u8>);
				child.readResource = [attributes, full, fork](u64 offset,
						void* buffer, size_t length) {
					if (offset == 0 || fork->empty()) {
						fork->clear();
						if (!attributes->ReadAttribute(full, kResourceFork,
								*fork))
							return false;
					}
					memset(buffer, 0, length);
					if (offset < fork->size()) {
						memcpy(buffer, fork->data() + offset,
							(size_t)std::min<u64>(length,
								fork->size() - offset));
					}
					if (offset + length >= fork->size())
						std::vector<u8>().swap(*fork);
					return true;
				};
			}
			record.hostSize = child.dataSize;
			record.dataSize = child.dataSize;
			record.resourceSize = child.resourceSize;
		}
		bytes += child.dataSize + child.resourceSize + 1024;
		fRecords[childHfs] = record;
		node.children.push_back(std::move(child));
	}
	return true;
}


bool
SharedMacFolder::_Build(std::string& error)
{
	struct stat st;
	if (stat(fFolder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
		error = "the shared folder " + fFolder + " does not exist";
		return false;
	}

	// Apple's driver, from a disk Drive Setup initialized
	ApplePartitionMap::Drivers drivers;
	bool found = false;
	for (const std::string& path : fDriverDisks) {
		std::string openError;
		std::unique_ptr<DiskImage> disk = DiskImage::Open(path, true,
			openError);
		if (disk && ApplePartitionMap::ReadDrivers(*disk, drivers)) {
			found = true;
			break;
		}
	}
	if (!found) {
		error = "The shared Mac folder " + fFolder + " needs the SCSI "
			"driver of a disk Drive Setup initialized (a Mac OS disk) in the "
			"same profile.";
		return false;
	}

	fRecords.clear();
	HfsWriter::Node root;
	root.directory = true;
	root.name = FitName(HostToMacName(Leaf(fFolder)), "", 27);
	if (root.name.empty())
		root.name = "Shared";
	root.created = ToMacTime(BirthTime(st));
	root.modified = ToMacTime(st.st_mtim.tv_sec);
	_FinderInfo(fFolder, Leaf(fFolder), true, false, root.finderInfo);
	u64 bytes = 0;
	if (!_Scan("", "", root, bytes, error))
		return false;
	Record rootRecord;
	rootRecord.directory = true;
	rootRecord.infoHash = HashInfo(root.finderInfo);
	fRecords[""] = rootRecord;

	// room to grow: twice the content and 512 MB, from 1 GB to 2 GB (HFS
	// on Mac OS 7.6.1)
	u64 size = std::max<u64>(1ull << 30, bytes * 2 + (512ull << 20));
	size = std::min<u64>(size, HfsWriter::kMaxVolumeSize);
	u32 hfsStart = drivers.end;
	for (;;) {
		unlink(fImage.c_str());
		if (!Qcow2Image::Create(fImage, (u64)hfsStart * 512 + size, error))
			return false;
		std::unique_ptr<DiskImage> image = DiskImage::Open(fImage, false,
			error);
		if (!image)
			return false;
		DiskImage* disk = image.get();
		bool written = ApplePartitionMap::WriteDisk(
				[disk](u64 offset, const void* buffer, size_t length) {
					return disk->Write(offset, buffer, length);
				}, drivers, (u32)(hfsStart + size / 512), hfsStart,
				root.name, error)
			&& HfsWriter::Write([disk, hfsStart](u64 offset,
					const void* buffer, size_t length) {
					return disk->Write((u64)hfsStart * 512 + offset, buffer,
						length);
				}, size, root, error);
		if (written && image->Flush())
			break;
		image.reset();
		unlink(fImage.c_str());
		if (size == HfsWriter::kMaxVolumeSize) {
			error = "The shared Mac folder " + fFolder + ": " + error
				+ " (HFS volumes for Mac OS 7.6.1 hold up to 2 GB).";
			return false;
		}
		size = HfsWriter::kMaxVolumeSize;
	}
	if (!_SaveSnapshot()) {
		error = "cannot write " + fImage + ".snapshot";
		return false;
	}
	return true;
}


bool
SharedMacFolder::_SaveSnapshot()
{
	std::string path = fImage + ".snapshot";
	FILE* file = fopen(path.c_str(), "w");
	if (file == nullptr)
		return false;
	fprintf(file, "PowerMac shared Mac folder 1\t%s\n",
		Escape(fFolder).c_str());
	for (const auto& item : fRecords) {
		const Record& record = item.second;
		fprintf(file, "%c\t%s\t%s\t%llu\t%lld\t%lu\t%llu\t%llu\t%llu\n",
			record.directory ? 'D' : 'F', Escape(item.first).c_str(),
			Escape(record.hostPath).c_str(),
			(unsigned long long)record.hostSize,
			(long long)record.hostModified, (unsigned long)record.macModified,
			(unsigned long long)record.dataSize,
			(unsigned long long)record.resourceSize,
			(unsigned long long)record.infoHash);
	}
	return fclose(file) == 0;
}


bool
SharedMacFolder::_LoadSnapshot()
{
	fRecords.clear();
	std::string path = fImage + ".snapshot";
	FILE* file = fopen(path.c_str(), "r");
	if (file == nullptr)
		return false;
	std::string text;
	char buffer[65536];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, got);
	fclose(file);
	static const char kHeader[] = "PowerMac shared Mac folder 1\t";
	const size_t headerLength = sizeof(kHeader) - 1;
	size_t position = text.find('\n');
	if (text.compare(0, headerLength, kHeader) != 0
		|| position == std::string::npos)
		return false;
	// the folder the volume was made from (the setting may have changed)
	fSnapshotFolder = Unescape(text.substr(headerLength,
		position - headerLength));
	position++;
	while (position < text.size()) {
		size_t end = text.find('\n', position);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(position, end - position);
		position = end + 1;
		std::vector<std::string> fields;
		size_t start = 0;
		for (;;) {
			size_t tab = line.find('\t', start);
			fields.push_back(line.substr(start, tab == std::string::npos
				? std::string::npos : tab - start));
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}
		if (fields.size() != 9)
			continue;
		Record record;
		record.directory = fields[0] == "D";
		record.hostPath = Unescape(fields[2]);
		record.hostSize = strtoull(fields[3].c_str(), nullptr, 10);
		record.hostModified = strtoll(fields[4].c_str(), nullptr, 10);
		record.macModified = (u32)strtoul(fields[5].c_str(), nullptr, 10);
		record.dataSize = strtoull(fields[6].c_str(), nullptr, 10);
		record.resourceSize = strtoull(fields[7].c_str(), nullptr, 10);
		record.infoHash = strtoull(fields[8].c_str(), nullptr, 10);
		fRecords[Unescape(fields[1])] = record;
	}
	return true;
}


bool
SharedMacFolder::Prepare(std::string& report, std::string& error)
{
	report.clear();
	struct stat st;
	bool leftOver = stat((fImage + ".snapshot").c_str(), &st) == 0
		&& stat(fImage.c_str(), &st) == 0;
	if (leftOver) {
		std::string syncReport;
		std::string syncError;
		if (!Sync(syncReport, syncError)) {
			// not cleanly unmounted: Mac OS gets the volume back to check
			if (_LoadSnapshot()) {
				report = syncError;
				return true;
			}
		} else
			report = syncReport;
	}
	return _Build(error);
}


// #pragma mark - syncing back


bool
SharedMacFolder::Sync(std::string& report, std::string& error)
{
	report.clear();
	if (!_LoadSnapshot()) {
		error = "no snapshot of the shared folder " + fFolder;
		return false;
	}
	std::unique_ptr<DiskImage> image = DiskImage::Open(fImage, true, error);
	if (!image)
		return false;
	std::string currentFolder = fFolder;
	fFolder = fSnapshotFolder;
	bool result = _Sync(*image, report, error);
	fFolder = currentFolder;
	image.reset();
	if (result) {
		unlink((fImage + ".snapshot").c_str());
		unlink(fImage.c_str());
	}
	return result;
}


// Finder information and, with 'contents', the resource fork and a MIME
// type (when the file has none).
void
SharedMacFolder::_WriteMacAttributes(const std::string& path,
	const HfsReader::Entry& entry, const HfsReader& reader, bool contents,
	bool newFile)
{
	if (fAttributes == nullptr)
		return;
	std::vector<BfsAttribute> attributes;
	BfsAttribute info;
	info.name = kFinderInfo;
	info.type = kRawType;
	info.data.assign(entry.finderInfo, entry.finderInfo + 32);
	attributes.push_back(info);
	BfsAttribute flags;
	flags.name = kFlags;
	flags.type = kUint16Type;
	flags.data.assign(entry.finderInfo + 8, entry.finderInfo + 10);
	attributes.push_back(flags);
	if (!entry.directory) {
		if (LoadBE32(entry.finderInfo + 4) != 0) {
			BfsAttribute creator;
			creator.name = kCreator;
			creator.type = kUint32Type;
			creator.data.assign(entry.finderInfo + 4, entry.finderInfo + 8);
			attributes.push_back(creator);
		}
		u64 size;
		if (contents && (entry.resourceSize > 0
				|| fAttributes->AttributeSize(path, kResourceFork, size))) {
			BfsAttribute fork;
			fork.name = kResourceFork;
			fork.type = kRawType;
			fork.data.resize((size_t)entry.resourceSize);
			if (!reader.ReadFork(entry.id, true, 0, fork.data.data(),
					fork.data.size()))
				fork.data.clear();
			attributes.push_back(fork);
		}
		const char* mime = MimeType(LoadBE32(entry.finderInfo));
		if (contents && newFile && mime[0] != '\0'
			&& !fAttributes->AttributeSize(path, kMimeAttribute, size)) {
			BfsAttribute type;
			type.name = kMimeAttribute;
			type.type = kMimeStringType;
			type.data.assign(mime, mime + strlen(mime) + 1);
			attributes.push_back(type);
		}
	}
	fAttributes->Write(path, attributes);
}


bool
SharedMacFolder::_Sync(DiskImage& image, std::string& report,
	std::string& error)
{
	DiskImage* disk = &image;
	HfsReader::ReadFunction read = [disk](u64 offset, void* buffer,
			size_t length) {
		return disk->Read(offset, buffer, length);
	};
	HfsReader reader(read);
	u64 offset;
	u64 size;
	if (!ApplePartitionMap::FindHfs(read, disk->Size(), offset, size)
		|| !reader.Mount(offset)) {
		error = "the shared volume of " + fFolder + " is damaged";
		return false;
	}
	if (!reader.IsClean()) {
		error = "Mac OS did not shut down: the changes to the shared folder "
			+ fFolder + " come back at the next power on, after Mac OS has "
			"used the volume again and shut down.";
		return false;
	}

	std::set<std::string> seen;
	int copied = 0;
	int deleted = 0;
	std::vector<std::string> conflicts;
	std::vector<u8> buffer(1 << 20);

	std::function<void(u32, const std::string&, const std::string&)> walk
		= [&](u32 directory, const std::string& hfsPath,
			const std::string& hostRelative) {
		std::vector<HfsReader::Entry> entries;
		reader.List(directory, entries);
		bool atRoot = directory == HfsReader::kRootId;
		for (const HfsReader::Entry& entry : entries) {
			std::string childHfs = hfsPath + ":" + entry.name;
			auto record = fRecords.find(childHfs);
			bool known = record != fRecords.end();
			if (atRoot && !known && IsHousekeeping(entry))
				continue;
			seen.insert(childHfs);
			std::string childRelative = hostRelative + "/"
				+ (known ? Leaf(record->second.hostPath)
					: SafeHostName(entry.name));
			std::string host = fFolder + childRelative;
			u64 infoHash = HashInfo(entry.finderInfo);
			struct stat hostStat;
			bool hostExists = lstat(host.c_str(), &hostStat) == 0;

			if (entry.directory) {
				bool made = false;
				if (!hostExists)
					made = mkdir(host.c_str(), 0755) == 0;
				if (made || !known || record->second.infoHash != infoHash)
					_WriteMacAttributes(host, entry, reader, false, made);
				walk(entry.id, childHfs, childRelative);
				// the Finder's Trash and Desktop Folder: kept while they
				// hold something
				if (made && atRoot && IsSpecialFolder(entry.name))
					rmdir(host.c_str());
				continue;
			}

			bool changed = !known || record->second.directory
				|| record->second.macModified != entry.modified
				|| record->second.dataSize != entry.dataSize
				|| record->second.resourceSize != entry.resourceSize;
			if (!changed) {
				if (record->second.infoHash != infoHash && hostExists)
					_WriteMacAttributes(host, entry, reader, false, false);
				continue;
			}
			std::string target = host;
			if (hostExists && (!known
					|| (u64)hostStat.st_size != record->second.hostSize
					|| ModifiedNs(hostStat) != record->second.hostModified)) {
				// changed on both sides: the host keeps its version
				target = ConflictName(host);
				conflicts.push_back(Leaf(target));
			}
			std::string temporary = target + ".powermac-sync";
			int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
				0644);
			if (fd < 0)
				continue;
			bool ok = true;
			for (u64 position = 0; position < entry.dataSize && ok; ) {
				size_t length = (size_t)std::min<u64>(buffer.size(),
					entry.dataSize - position);
				ok = reader.ReadFork(entry.id, false, position, buffer.data(),
						length)
					&& write(fd, buffer.data(), length) == (ssize_t)length;
				position += length;
			}
			ok = close(fd) == 0 && ok;
			// the host's own attributes (its MIME type, Tracker's) stay
			if (ok && target == host && hostExists && fAttributes != nullptr) {
				std::vector<BfsAttribute> attributes;
				if (fAttributes->Read(host, attributes)) {
					attributes.erase(std::remove_if(attributes.begin(),
						attributes.end(), [](const BfsAttribute& attribute) {
							return attribute.name == kResourceFork;
						}), attributes.end());
					fAttributes->Write(temporary, attributes);
				}
			}
			if (!ok || rename(temporary.c_str(), target.c_str()) != 0) {
				unlink(temporary.c_str());
				continue;
			}
			struct timeval times[2];
			times[0].tv_sec = times[1].tv_sec
				= (time_t)FromMacTime(entry.modified);
			times[0].tv_usec = times[1].tv_usec = 0;
			utimes(target.c_str(), times);
			_WriteMacAttributes(target, entry, reader, true, true);
			copied++;
		}
	};
	walk(HfsReader::kRootId, "", "");

	// the volume's own Finder information (its window)
	std::vector<HfsReader::Entry> roots;
	reader.List(1, roots);
	auto rootRecord = fRecords.find("");
	if (roots.size() == 1 && rootRecord != fRecords.end()
		&& rootRecord->second.infoHash != HashInfo(roots[0].finderInfo))
		_WriteMacAttributes(fFolder, roots[0], reader, false, false);

	// deleted in Mac OS: files unchanged on the host since power on, then
	// emptied folders (deepest first)
	std::vector<const Record*> gone;
	for (const auto& item : fRecords) {
		if (!item.first.empty() && seen.count(item.first) == 0)
			gone.push_back(&item.second);
	}
	std::sort(gone.begin(), gone.end(), [](const Record* a, const Record* b) {
		return a->hostPath.size() > b->hostPath.size();
	});
	for (const Record* record : gone) {
		std::string host = fFolder + record->hostPath;
		struct stat hostStat;
		if (lstat(host.c_str(), &hostStat) != 0)
			continue;
		if (record->directory) {
			if (S_ISDIR(hostStat.st_mode) && rmdir(host.c_str()) == 0)
				deleted++;
		} else if ((u64)hostStat.st_size == record->hostSize
			&& ModifiedNs(hostStat) == record->hostModified
			&& unlink(host.c_str()) == 0)
			deleted++;
	}

	if (copied > 0 || deleted > 0 || !conflicts.empty()) {
		report = "Shared folder \"" + Leaf(fFolder) + "\": "
			+ std::to_string(copied) + " file(s) from Mac OS, "
			+ std::to_string(deleted) + " removed";
		if (!conflicts.empty()) {
			report += "; changed on both sides, Mac OS's version kept as \""
				+ conflicts[0] + "\"";
			if (conflicts.size() > 1)
				report += " and " + std::to_string(conflicts.size() - 1)
					+ " more";
		}
		report += ".";
	}
	return true;
}

}	// namespace powermac
