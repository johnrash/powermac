/*
 * PowerMac emulator - Apple partition maps.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "ApplePartitionMap.h"

#include "DiskImage.h"

#include <algorithm>
#include <cstring>

namespace powermac {

namespace {

const u32 kMapBlocks = 63;			// the map: blocks 1-63
const u32 kMaxEntries = 63;
const u32 kDefaultHfsStatus = 0x4000037f;

// partition entry fields
const int kEntryCount = 4;			// pmMapBlkCnt
const int kStart = 8;				// pmPyPartStart
const int kCount = 12;				// pmPartBlkCnt
const int kName = 16;				// pmPartName[32]
const int kType = 48;				// pmParType[32]
const int kDataCount = 84;			// pmDataCnt
const int kStatus = 88;				// pmPartStatus


std::string
Field(const u8* entry, int offset)
{
	const char* text = (const char*)entry + offset;
	return std::string(text, strnlen(text, 32));
}


void
SetField(u8* entry, int offset, const std::string& text)
{
	memset(entry + offset, 0, 32);
	memcpy(entry + offset, text.data(), std::min<size_t>(31, text.size()));
}


void
Entry(u8* entry, u32 start, u32 count, const std::string& name,
	const std::string& type, u32 status)
{
	memset(entry, 0, 512);
	StoreBE16(entry, 0x504d);			// 'PM'
	StoreBE32(entry + kStart, start);
	StoreBE32(entry + kCount, count);
	SetField(entry, kName, name);
	SetField(entry, kType, type);
	StoreBE32(entry + kDataCount, count);
	StoreBE32(entry + kStatus, status);
}

}	// namespace


bool
ApplePartitionMap::ReadDrivers(DiskImage& disk, Drivers& drivers)
{
	drivers = Drivers();
	u8* map = drivers.descriptorMap;
	if (!disk.Read(0, map, 512) || LoadBE16(map) != 0x4552
		|| LoadBE16(map + 2) != 512)
		return false;
	u16 driverCount = LoadBE16(map + 16);
	if (driverCount == 0 || driverCount > 60)
		return false;

	u8 entry[512];
	u32 entries = 1;
	drivers.hfsStatus = kDefaultHfsStatus;
	bool foundHfs = false;
	for (u32 i = 1; i <= entries && i <= kMaxEntries; i++) {
		if (!disk.Read((u64)i * 512, entry, sizeof(entry))
			|| LoadBE16(entry) != 0x504d)
			return false;
		if (i == 1)
			entries = LoadBE32(entry + kEntryCount);
		std::string type = Field(entry, kType);
		u32 start = LoadBE32(entry + kStart);
		u32 count = LoadBE32(entry + kCount);
		if (type == "Apple_HFS" && !foundHfs) {
			drivers.hfsStatus = LoadBE32(entry + kStatus);
			foundHfs = true;
		}
		if (type.compare(0, 12, "Apple_Driver") != 0
			&& type != "Apple_Patches" && type != "Apple_FWDriver")
			continue;
		// after the map and near the start, as Drive Setup puts them
		if (start <= kMapBlocks || count == 0 || start + count > 65536)
			return false;
		Drivers::Partition partition;
		memcpy(partition.entry, entry, sizeof(entry));
		partition.start = start;
		partition.count = count;
		partition.data.resize((size_t)count * 512);
		if (!disk.Read((u64)start * 512, partition.data.data(),
				partition.data.size()))
			return false;
		drivers.partitions.push_back(partition);
		drivers.end = std::max(drivers.end, start + count);
	}

	// each driver the map names must be in a partition taken
	for (u16 i = 0; i < driverCount; i++) {
		u32 block = LoadBE32(map + 18 + 8 * i);
		bool found = false;
		for (const Drivers::Partition& partition : drivers.partitions)
			found |= partition.start == block;
		if (!found)
			return false;
	}
	return !drivers.partitions.empty();
}


bool
ApplePartitionMap::WriteDisk(WriteFunction write, const Drivers& drivers,
	u32 blocks, u32 hfsStart, const std::string& name, std::string& error)
{
	if (hfsStart < drivers.end || hfsStart <= kMapBlocks || hfsStart >= blocks) {
		error = "the HFS partition does not fit the disk";
		return false;
	}
	u8 map[512];
	memcpy(map, drivers.descriptorMap, sizeof(map));
	StoreBE32(map + 4, blocks);
	if (!write(0, map, sizeof(map))) {
		error = "cannot write the disk";
		return false;
	}

	u32 entries = 1 + (u32)drivers.partitions.size() + 1;
	std::vector<u8> table((size_t)kMapBlocks * 512);
	Entry(&table[0], 1, kMapBlocks, "Apple", "Apple_partition_map", 0x37);
	for (size_t i = 0; i < drivers.partitions.size(); i++)
		memcpy(&table[(i + 1) * 512], drivers.partitions[i].entry, 512);
	Entry(&table[(entries - 1) * 512], hfsStart, blocks - hfsStart, name,
		"Apple_HFS", drivers.hfsStatus);
	for (u32 i = 0; i < entries; i++)
		StoreBE32(&table[i * 512 + kEntryCount], entries);
	if (!write(512, table.data(), table.size())) {
		error = "cannot write the partition map";
		return false;
	}
	for (const Drivers::Partition& partition : drivers.partitions) {
		if (!write((u64)partition.start * 512, partition.data.data(),
				partition.data.size())) {
			error = "cannot write the disk's driver";
			return false;
		}
	}
	return true;
}


bool
ApplePartitionMap::FindHfs(ReadFunction read, u64 diskSize, u64& offset,
	u64& size)
{
	u8 block[512];
	if (!read(0, block, sizeof(block)))
		return false;
	if (LoadBE16(block) != 0x4552) {
		if (!read(1024, block, sizeof(block)) || LoadBE16(block) != 0x4244)
			return false;
		offset = 0;
		size = diskSize;
		return true;
	}
	u32 entries = 1;
	for (u32 i = 1; i <= entries && i <= kMaxEntries; i++) {
		if (!read((u64)i * 512, block, sizeof(block))
			|| LoadBE16(block) != 0x504d)
			return false;
		if (i == 1)
			entries = LoadBE32(block + kEntryCount);
		if (Field(block, kType) == "Apple_HFS") {
			offset = (u64)LoadBE32(block + kStart) * 512;
			size = (u64)LoadBE32(block + kCount) * 512;
			return true;
		}
	}
	return false;
}

}	// namespace powermac
