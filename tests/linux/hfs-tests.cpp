/*
 * PowerMac emulator - HFS tests: Mac OS Roman names and their order;
 * volumes HfsWriter writes read back with HfsReader (a catalog big enough
 * for index levels and a map node, both forks), and by hfsutils when it is
 * installed; partition maps with a driver; a folder shared with Mac OS,
 * changed "by Mac OS" (the volume rewritten) and synced back.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 *   hfs-tests WORKDIR
 */
#include "core/storage/ApplePartitionMap.h"
#include "core/storage/DiskImage.h"
#include "core/storage/HfsReader.h"
#include "core/storage/HfsWriter.h"
#include "core/storage/MacRoman.h"
#include "core/storage/SharedMacFolder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

using namespace powermac;

namespace {

int sFailures = 0;

void
Check(bool ok, const std::string& what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
	if (!ok)
		sFailures++;
}


u8
Pattern(const std::string& name, u64 offset)
{
	u32 hash = 2166136261u;
	for (char c : name)
		hash = (hash ^ (u8)c) * 16777619u;
	return (u8)(hash + offset * 7 + (offset >> 9));
}


HfsWriter::ReadFunction
PatternReader(const std::string& name)
{
	return [name](u64 offset, void* buffer, size_t length) {
		u8* bytes = (u8*)buffer;
		for (size_t i = 0; i < length; i++)
			bytes[i] = Pattern(name, offset + i);
		return true;
	};
}


HfsWriter::ReadFunction
BytesReader(const std::vector<u8>& data)
{
	return [data](u64 offset, void* buffer, size_t length) {
		memcpy(buffer, data.data() + offset, length);
		return true;
	};
}


HfsWriter::Node
File(const std::string& name, u64 dataSize, u64 resourceSize = 0,
	const char* type = "TEXT", const char* creator = "ttxt")
{
	HfsWriter::Node node;
	node.name = name;
	node.dataSize = dataSize;
	node.readData = PatternReader(name);
	node.resourceSize = resourceSize;
	node.readResource = PatternReader(name + "/rsrc");
	node.created = 3000000000u;
	node.modified = 3000000000u + (u32)dataSize;
	memcpy(node.finderInfo, type, 4);
	memcpy(node.finderInfo + 4, creator, 4);
	return node;
}


HfsWriter::Node
Folder(const std::string& name)
{
	HfsWriter::Node node;
	node.name = name;
	node.directory = true;
	node.created = node.modified = 3000000000u;
	return node;
}


bool
SameFork(const HfsReader& reader, u32 id, bool resource,
	const std::string& name, u64 size)
{
	std::vector<u8> buffer(1 << 20);
	for (u64 offset = 0; offset < size; ) {
		size_t length = (size_t)std::min<u64>(buffer.size(), size - offset);
		if (!reader.ReadFork(id, resource, offset, buffer.data(), length))
			return false;
		for (size_t i = 0; i < length; i++) {
			if (buffer[i] != Pattern(name, offset + i))
				return false;
		}
		offset += length;
	}
	return true;
}


const HfsReader::Entry*
FindEntry(const std::vector<HfsReader::Entry>& entries,
	const std::string& name)
{
	for (const HfsReader::Entry& entry : entries) {
		if (entry.name == name)
			return &entry;
	}
	return nullptr;
}


// Attributes kept in memory.
class FakeAttributes : public HostAttributes {
public:
	virtual bool Read(const std::string& path,
		std::vector<BfsAttribute>& attributes)
	{
		attributes = store[path];
		return true;
	}

	virtual bool Write(const std::string& path,
		const std::vector<BfsAttribute>& attributes)
	{
		for (const BfsAttribute& attribute : attributes) {
			std::vector<BfsAttribute>& list = store[path];
			bool replaced = false;
			for (BfsAttribute& existing : list) {
				if (existing.name == attribute.name) {
					existing = attribute;
					replaced = true;
				}
			}
			if (!replaced)
				list.push_back(attribute);
		}
		return true;
	}

	const std::vector<u8>* Find(const std::string& path,
		const std::string& name)
	{
		for (const BfsAttribute& attribute : store[path]) {
			if (attribute.name == name)
				return &attribute.data;
		}
		return nullptr;
	}

	std::map<std::string, std::vector<BfsAttribute> > store;
};


void
WriteText(const std::string& path, const std::string& text)
{
	std::ofstream(path) << text;
}


std::string
ReadText(const std::string& path)
{
	std::ifstream file(path);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}


bool
Exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}


std::vector<u8>
Bytes(const std::string& text)
{
	return std::vector<u8>(text.begin(), text.end());
}


void
TestNames()
{
	Check(HostToMacName("caf\xc3\xa9") == "caf\x8e"
		&& MacToHostName("caf\x8e") == "caf\xc3\xa9", "Mac OS Roman é");
	Check(HostToMacName("a:b") == "a/b" && MacToHostName("a/b") == "a:b",
		"colon and slash trade places");
	Check(HostToMacName("\xe6\x97\xa5x") == "_x", "no Mac OS Roman: _");
	Check(HostToMacName("\xe2\x82\xac") == "\xdb", "euro");
	Check(CompareHfsNames("readme", "README") == 0, "case does not count");
	Check(CompareHfsNames("\x80", "\x8a") == 0, "nor for Ä and ä");
	Check(CompareHfsNames("a", "\x87") < 0 && CompareHfsNames("\x87", "b") < 0,
		"á between a and b");
	Check(CompareHfsNames("a", "b") < 0 && CompareHfsNames("ab", "a") > 0
		&& CompareHfsNames("_", "a") > 0, "order");
}


void
TestVolume(const std::string& work)
{
	std::string path = work + "/volume.img";
	unlink(path.c_str());
	const u64 size = 64ull << 20;
	std::string error;
	Check(RawImage::Create(path, size, error), "image created");
	std::unique_ptr<RawImage> image = RawImage::Open(path, false, error);
	if (!image)
		return;

	HfsWriter::Node root = Folder("Test Volume");
	HfsWriter::Node many = Folder("Many");
	// enough files for two index levels and a map node
	for (int i = 0; i < 6000; i++) {
		char name[32];
		snprintf(name, sizeof(name), "file %04d", i);
		many.children.push_back(File(name, i % 7 == 0 ? 600 : 10,
			i % 5 == 0 ? 300 : 0));
	}
	HfsWriter::Node nested = Folder("Nested");
	HfsWriter::Node deeper = Folder("Deeper");
	deeper.children.push_back(File("large.bin", 3 * 1024 * 1024 + 17));
	nested.children.push_back(deeper);
	nested.children.push_back(File("empty", 0));
	root.children.push_back(many);
	root.children.push_back(nested);
	root.children.push_back(File("Resource only", 0, 5000, "APPL", "ABCD"));
	root.children.push_back(File("caf\x8e", 3));
	RawImage* raw = image.get();
	Check(HfsWriter::Write([raw](u64 offset, const void* buffer,
			size_t length) {
			return raw->Write(offset, buffer, length);
		}, size, root, error), "volume written" + (error.empty()
			? std::string() : ": " + error));

	HfsReader reader([raw](u64 offset, void* buffer, size_t length) {
		return raw->Read(offset, buffer, length);
	});
	Check(reader.Mount(0) && reader.IsClean()
		&& reader.VolumeName() == "Test Volume", "volume mounted");
	std::vector<HfsReader::Entry> top;
	reader.List(HfsReader::kRootId, top);
	Check(top.size() == 4 && top[0].name == "caf\x8e"
		&& top[1].name == "Many" && top[2].name == "Nested"
		&& top[3].name == "Resource only", "root folder, in HFS order");
	const HfsReader::Entry* manyEntry = FindEntry(top, "Many");
	std::vector<HfsReader::Entry> files;
	if (manyEntry != nullptr)
		reader.List(manyEntry->id, files);
	bool filesOk = files.size() == 6000;
	for (size_t i = 0; filesOk && i < files.size(); i += 97) {
		char name[32];
		snprintf(name, sizeof(name), "file %04d", (int)i);
		const HfsReader::Entry& entry = files[i];
		filesOk = entry.name == name && !entry.directory
			&& entry.dataSize == (i % 7 == 0 ? 600u : 10u)
			&& entry.resourceSize == (i % 5 == 0 ? 300u : 0u)
			&& memcmp(entry.finderInfo, "TEXTttxt", 8) == 0
			&& SameFork(reader, entry.id, false, name, entry.dataSize)
			&& SameFork(reader, entry.id, true, std::string(name) + "/rsrc",
				entry.resourceSize);
	}
	Check(filesOk, "6000 files: names, sizes, Finder info, both forks");
	const HfsReader::Entry* nestedEntry = FindEntry(top, "Nested");
	std::vector<HfsReader::Entry> nestedList;
	std::vector<HfsReader::Entry> deeperList;
	if (nestedEntry != nullptr)
		reader.List(nestedEntry->id, nestedList);
	const HfsReader::Entry* deeperEntry = FindEntry(nestedList, "Deeper");
	if (deeperEntry != nullptr)
		reader.List(deeperEntry->id, deeperList);
	Check(deeperList.size() == 1 && deeperList[0].name == "large.bin"
		&& SameFork(reader, deeperList[0].id, false, "large.bin",
			3 * 1024 * 1024 + 17), "3 MB file in a nested folder");
	const HfsReader::Entry* resourceOnly = FindEntry(top, "Resource only");
	Check(resourceOnly != nullptr && resourceOnly->dataSize == 0
		&& SameFork(reader, resourceOnly->id, true, "Resource only/rsrc",
			5000) && memcmp(resourceOnly->finderInfo, "APPLABCD", 8) == 0,
		"resource fork only");

	HfsWriter::Node clash = Folder("Clash");
	clash.children.push_back(File("Name", 1));
	clash.children.push_back(File("NAME", 1));
	Check(!HfsWriter::Write([raw](u64 offset, const void* buffer,
			size_t length) {
			return raw->Write(offset, buffer, length);
		}, size, clash, error), "names differing by case refused");

	// hfsutils, when installed, reads the volume as well
	image.reset();
	if (system("command -v hls >/dev/null 2>&1") == 0) {
		std::string command = "cd '" + work + "' && HOME='" + work
			+ "' hmount volume.img >/dev/null && HOME='" + work
			+ "' hls -a :Many | wc -l";
		FILE* pipe = popen(command.c_str(), "r");
		char line[64] = "";
		if (pipe != nullptr) {
			if (fgets(line, sizeof(line), pipe) == nullptr)
				line[0] = '\0';
			pclose(pipe);
		}
		Check(atoi(line) >= 6000, std::string("hfsutils lists the files (")
			+ std::to_string(atoi(line)) + ")");
		system(("HOME='" + work + "' humount >/dev/null 2>&1").c_str());
	} else
		printf("skip: hfsutils not installed\n");
	unlink(path.c_str());
}


// A disk as Drive Setup 1.1 initializes one: two drivers and patches.
std::string
MakeDriverDisk(const std::string& work)
{
	std::string path = work + "/driver-disk.img";
	unlink(path.c_str());
	std::string error;
	RawImage::Create(path, 1 << 20, error);
	std::unique_ptr<RawImage> disk = RawImage::Open(path, false, error);
	u8 block[512] = {};
	StoreBE16(block, 0x4552);
	StoreBE16(block + 2, 512);
	StoreBE32(block + 4, 2048);
	StoreBE16(block + 16, 2);
	StoreBE32(block + 18, 64);
	StoreBE16(block + 22, 18);
	StoreBE16(block + 24, 1);
	StoreBE32(block + 26, 118);
	StoreBE16(block + 30, 32);
	StoreBE16(block + 32, 0xffff);
	disk->Write(0, block, sizeof(block));
	struct {
		u32 start;
		u32 count;
		const char* name;
		const char* type;
		u32 status;
	} entries[] = {
		{ 1, 63, "Apple", "Apple_partition_map", 0x37 },
		{ 64, 54, "Macintosh", "Apple_Driver43", 0x17f },
		{ 118, 74, "Macintosh", "Apple_Driver43", 0x27f },
		{ 192, 512, "Patch Partition", "Apple_Patches", 0x37 },
		{ 704, 1344, "untitled", "Apple_HFS", 0x4000037f },
	};
	for (int i = 0; i < 5; i++) {
		memset(block, 0, sizeof(block));
		StoreBE16(block, 0x504d);
		StoreBE32(block + 4, 5);
		StoreBE32(block + 8, entries[i].start);
		StoreBE32(block + 12, entries[i].count);
		strcpy((char*)block + 16, entries[i].name);
		strcpy((char*)block + 48, entries[i].type);
		StoreBE32(block + 84, entries[i].count);
		StoreBE32(block + 88, entries[i].status);
		disk->Write((u64)(i + 1) * 512, block, sizeof(block));
	}
	for (u32 b = 64; b < 704; b++) {
		for (int i = 0; i < 512; i++)
			block[i] = Pattern("driver", b * 512 + i);
		disk->Write((u64)b * 512, block, sizeof(block));
	}
	return path;
}


void
TestPartitionMap(const std::string& work)
{
	std::string error;
	std::string driverDisk = MakeDriverDisk(work);
	std::unique_ptr<DiskImage> source = DiskImage::Open(driverDisk, true,
		error);
	ApplePartitionMap::Drivers drivers;
	Check(source && ApplePartitionMap::ReadDrivers(*source, drivers)
		&& drivers.partitions.size() == 3 && drivers.end == 704
		&& drivers.hfsStatus == 0x4000037f, "drivers of a Drive Setup disk");

	std::vector<u8> disk(4096 * 512);
	auto write = [&disk](u64 offset, const void* buffer, size_t length) {
		if (offset + length > disk.size())
			return false;
		memcpy(disk.data() + offset, buffer, length);
		return true;
	};
	Check(ApplePartitionMap::WriteDisk(write, drivers, 4096, 704, "Shared",
		error), "disk written");
	bool copied = LoadBE16(disk.data()) == 0x4552
		&& LoadBE32(disk.data() + 4) == 4096
		&& LoadBE32(disk.data() + 512 + 4) == 5;
	for (u32 offset = 64 * 512; copied && offset < 704 * 512; offset += 509)
		copied = disk[offset] == Pattern("driver", offset);
	Check(copied, "driver map and drivers copied");
	u64 offset = 0;
	u64 size = 0;
	Check(ApplePartitionMap::FindHfs([&disk](u64 at, void* buffer,
			size_t length) {
			memcpy(buffer, disk.data() + at, length);
			return true;
		}, disk.size(), offset, size) && offset == 704 * 512
		&& size == (4096 - 704) * 512, "HFS partition found");

	std::vector<u8> blank(4096);
	std::string blankPath = work + "/blank.img";
	RawImage::Create(blankPath, 1 << 20, error);
	std::unique_ptr<DiskImage> blankDisk = DiskImage::Open(blankPath, true,
		error);
	Check(blankDisk && !ApplePartitionMap::ReadDrivers(*blankDisk, drivers),
		"a blank disk has no driver");
	unlink(blankPath.c_str());
}


struct Volume {
	std::unique_ptr<DiskImage> disk;
	u64 offset = 0;
	u64 size = 0;
	std::unique_ptr<HfsReader> reader;
};


bool
OpenVolume(const std::string& path, bool writable, Volume& volume)
{
	std::string error;
	volume.disk = DiskImage::Open(path, !writable, error);
	if (!volume.disk)
		return false;
	DiskImage* disk = volume.disk.get();
	HfsReader::ReadFunction read = [disk](u64 offset, void* buffer,
			size_t length) {
		return disk->Read(offset, buffer, length);
	};
	volume.reader.reset(new HfsReader(read));
	return ApplePartitionMap::FindHfs(read, disk->Size(), volume.offset,
			volume.size)
		&& volume.reader->Mount(volume.offset);
}


// The tree of a volume, forks in memory: what "Mac OS" changes.
HfsWriter::Node
ReadTree(const HfsReader& reader, const HfsReader::Entry& directory)
{
	HfsWriter::Node node;
	node.name = directory.name;
	node.directory = true;
	node.created = directory.created;
	node.modified = directory.modified;
	memcpy(node.finderInfo, directory.finderInfo, 32);
	std::vector<HfsReader::Entry> entries;
	reader.List(directory.id, entries);
	for (const HfsReader::Entry& entry : entries) {
		if (entry.directory) {
			node.children.push_back(ReadTree(reader, entry));
			continue;
		}
		HfsWriter::Node file;
		file.name = entry.name;
		file.created = entry.created;
		file.modified = entry.modified;
		memcpy(file.finderInfo, entry.finderInfo, 32);
		std::vector<u8> data((size_t)entry.dataSize);
		std::vector<u8> resource((size_t)entry.resourceSize);
		reader.ReadFork(entry.id, false, 0, data.data(), data.size());
		reader.ReadFork(entry.id, true, 0, resource.data(), resource.size());
		file.dataSize = data.size();
		file.readData = BytesReader(data);
		file.resourceSize = resource.size();
		file.readResource = BytesReader(resource);
		node.children.push_back(file);
	}
	return node;
}


HfsWriter::Node*
Child(HfsWriter::Node& node, const std::string& name)
{
	for (HfsWriter::Node& child : node.children) {
		if (child.name == name)
			return &child;
	}
	return nullptr;
}


void
SetContent(HfsWriter::Node& file, const std::string& text)
{
	file.dataSize = text.size();
	file.readData = BytesReader(Bytes(text));
	file.modified += 100;
}


void
TestSharedMacFolder(const std::string& work)
{
	std::string folder = work + "/Shared Stuff";
	std::string image = work + "/shared-mac.qcow2";
	system(("rm -rf '" + folder + "' '" + image + "' '" + image
		+ ".snapshot'").c_str());
	mkdir(folder.c_str(), 0755);
	mkdir((folder + "/sub").c_str(), 0755);
	mkdir((folder + "/Trash").c_str(), 0755);
	WriteText(folder + "/Read Me.txt", "hello");
	WriteText(folder + "/photo.JPG", "jpeg data");
	WriteText(folder + "/sub/notes", "notes");
	WriteText(folder + "/app", "");
	WriteText(folder + "/README", "upper");
	WriteText(folder + "/readme", "lower");
	WriteText(folder + "/A very long file name that goes on and on.txt",
		"long");
	WriteText(folder + "/a:b", "colon");

	FakeAttributes attributes;
	u8 info[32] = {};
	memcpy(info, "TEXTttxt", 8);
	StoreBE16(info + 8, 0x0100);
	attributes.store[folder + "/sub/notes"] = {
		{ "MACOS:FINDER_INFO", 0x52415754, std::vector<u8>(info, info + 32) } };
	attributes.store[folder + "/app"] = {
		{ "MACOS:RFORK", 0x52415754, Bytes("resource fork") },
		{ "MACOS:CREATOR", 0x554c4e47, Bytes("WXYZ") },
		{ "BEOS:TYPE", 0x4d494d53, Bytes("application/x-MacOS-APPL") } };

	std::string driverDisk = MakeDriverDisk(work);
	SharedMacFolder missing(folder, image, { work + "/none.img" },
		&attributes);
	std::string report, error;
	Check(!missing.Prepare(report, error)
		&& error.find("Drive Setup") != std::string::npos,
		"no driver disk: refused");

	SharedMacFolder share(folder, image, { work + "/none.img", driverDisk },
		&attributes);
	error.clear();
	Check(share.Prepare(report, error), "shared Mac volume built"
		+ (error.empty() ? std::string() : ": " + error));

	HfsWriter::Node changed;
	{
		Volume volume;
		Check(OpenVolume(image, false, volume)
			&& volume.offset == 704 * 512 && volume.reader->IsClean()
			&& volume.reader->VolumeName() == "Shared Stuff",
			"volume in the partition map");
		const HfsReader& reader = *volume.reader;
		std::vector<HfsReader::Entry> top;
		reader.List(HfsReader::kRootId, top);
		std::string names;
		for (const HfsReader::Entry& entry : top)
			names += entry.name + "|";
		Check(names == "A very long file name that.txt|a/b|app|photo.JPG|"
			"Read Me.txt|README|readme#1|sub|Trash|", "names: " + names);
		const HfsReader::Entry* photo = FindEntry(top, "photo.JPG");
		const HfsReader::Entry* app = FindEntry(top, "app");
		const HfsReader::Entry* trash = FindEntry(top, "Trash");
		Check(photo != nullptr
			&& memcmp(photo->finderInfo, "JPEGogle", 8) == 0,
			"type and creator from the extension");
		std::vector<u8> fork(13);
		Check(app != nullptr && memcmp(app->finderInfo, "APPLWXYZ", 8) == 0
			&& app->resourceSize == 13
			&& reader.ReadFork(app->id, true, 0, fork.data(), fork.size())
			&& fork == Bytes("resource fork"),
			"resource fork and creator from attributes");
		Check(trash != nullptr
			&& (LoadBE16(trash->finderInfo + 8) & 0x4000) != 0,
			"Trash invisible");
		std::vector<HfsReader::Entry> subList;
		const HfsReader::Entry* sub = FindEntry(top, "sub");
		if (sub != nullptr)
			reader.List(sub->id, subList);
		Check(subList.size() == 1
			&& memcmp(subList[0].finderInfo, info, 32) == 0,
			"Finder info from the attribute");

		// "Mac OS" at work
		std::vector<HfsReader::Entry> roots;
		reader.List(1, roots);
		changed = ReadTree(reader, roots[0]);
	}
	SetContent(*Child(changed, "Read Me.txt"), "hello from Mac OS");
	StoreBE32(Child(changed, "photo.JPG")->finderInfo + 10, 0x00400080);
	SetContent(*Child(changed, "readme#1"), "lower from Mac OS");
	SetContent(*Child(changed, "README"), "upper from Mac OS");
	Child(changed, "sub")->children.clear();
	HfsWriter::Node newDocument;
	newDocument.name = "New Doc";
	memcpy(newDocument.finderInfo, "TEXTttxt", 8);
	newDocument.modified = 3100000000u;
	SetContent(newDocument, "new");
	newDocument.resourceSize = 4;
	newDocument.readResource = BytesReader(Bytes("RSRC"));
	changed.children.push_back(newDocument);
	HfsWriter::Node slash = newDocument;
	slash.name = "x/y";
	slash.resourceSize = 0;
	changed.children.push_back(slash);
	HfsWriter::Node desktopDb = slash;
	desktopDb.name = "Desktop DB";
	StoreBE16(desktopDb.finderInfo + 8, 0x4000);
	changed.children.push_back(desktopDb);
	HfsWriter::Node temporary = Folder("Temporary Items");
	StoreBE16(temporary.finderInfo + 8, 0x4000);
	temporary.children.push_back(slash);
	changed.children.push_back(temporary);
	HfsWriter::Node desktop = Folder("Desktop Folder");
	StoreBE16(desktop.finderInfo + 8, 0x4000);
	changed.children.push_back(desktop);
	{
		Volume volume;
		Check(OpenVolume(image, true, volume), "volume reopened");
		DiskImage* disk = volume.disk.get();
		u64 offset = volume.offset;
		Check(HfsWriter::Write([disk, offset](u64 at, const void* buffer,
				size_t length) {
				return disk->Write(offset + at, buffer, length);
			}, volume.size, changed, error), "volume changed by Mac OS");
	}
	// the host changes README meanwhile
	sleep(1);
	WriteText(folder + "/README", "upper changed on the host");

	error.clear();
	Check(share.Sync(report, error), "sync" + (error.empty()
		? std::string() : ": " + error));
	printf("  report: %s\n", report.c_str());
	Check(ReadText(folder + "/Read Me.txt") == "hello from Mac OS",
		"changed file synced");
	Check(ReadText(folder + "/readme") == "lower from Mac OS"
		&& !Exists(folder + "/readme#1"), "\"readme#1\" goes back to readme");
	Check(ReadText(folder + "/README") == "upper changed on the host"
		&& ReadText(folder + "/README (PowerMac)") == "upper from Mac OS",
		"conflict keeps both versions");
	Check(!Exists(folder + "/sub/notes") && Exists(folder + "/sub"),
		"deleted file removed");
	Check(ReadText(folder + "/x:y") == "new", "slash becomes a colon");
	const std::vector<u8>* fork = attributes.Find(folder + "/New Doc",
		"MACOS:RFORK");
	const std::vector<u8>* mime = attributes.Find(folder + "/New Doc",
		"BEOS:TYPE");
	Check(ReadText(folder + "/New Doc") == "new" && fork != nullptr
		&& *fork == Bytes("RSRC") && mime != nullptr
		&& std::string((const char*)mime->data()) == "text/plain",
		"new file: resource fork and MIME type");
	const std::vector<u8>* photoInfo = attributes.Find(folder + "/photo.JPG",
		"MACOS:FINDER_INFO");
	Check(photoInfo != nullptr && photoInfo->size() == 32
		&& LoadBE32(photoInfo->data() + 10) == 0x00400080
		&& ReadText(folder + "/photo.JPG") == "jpeg data",
		"moved icon: Finder info only");
	Check(!Exists(folder + "/Desktop DB") && !Exists(folder + "/Temporary Items")
		&& !Exists(folder + "/Desktop Folder") && Exists(folder + "/Trash"),
		"the Finder's own files stay out");
	Check(!Exists(image) && !Exists(image + ".snapshot"), "volume removed");

	// built again: a volume left unmounted is kept for Mac OS
	Check(share.Prepare(report, error) && report.empty(), "built again");
	{
		Volume volume;
		Check(OpenVolume(image, true, volume), "volume opened");
		u8 attributesWord[2] = { 0, 0 };
		volume.disk->Write(volume.offset + 1024 + 10, attributesWord, 2);
	}
	Check(!share.Sync(report, error)
		&& error.find("did not shut down") != std::string::npos,
		"unclean volume not synced");
	Check(share.Prepare(report, error) && !report.empty() && Exists(image),
		"unclean volume kept for Mac OS");
	{
		Volume volume;
		Check(OpenVolume(image, false, volume) && !volume.reader->IsClean(),
			"the same volume");
	}
}

}	// namespace


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: hfs-tests WORKDIR\n");
		return 2;
	}
	char* absolute = realpath(argv[1], nullptr);
	if (absolute == nullptr) {
		perror(argv[1]);
		return 2;
	}
	std::string work = absolute;
	free(absolute);
	TestNames();
	TestVolume(work);
	TestPartitionMap(work);
	TestSharedMacFolder(work);
	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
