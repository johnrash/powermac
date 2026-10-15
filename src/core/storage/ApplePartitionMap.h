/*
 * PowerMac emulator - Apple partition maps.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * A disk Mac OS mounts starts with a driver descriptor map (block 0, "ER")
 * naming the blocks of its SCSI driver, then the partition map ("PM"
 * entries, a block each from block 1). The ROM loads the driver from the
 * disk at startup and the driver mounts the disk's HFS partitions; without
 * one Mac OS ignores the disk. Drive Setup writes Apple's driver (two
 * Apple_Driver43 partitions and an Apple_Patches one from block 64); the
 * emulator ships none, so a shared Mac volume gets a copy of those of a
 * disk Drive Setup initialized.
 */
#ifndef PM_APPLE_PARTITION_MAP_H
#define PM_APPLE_PARTITION_MAP_H

#include "../Types.h"

#include <functional>
#include <string>
#include <vector>

namespace powermac {

class DiskImage;

class ApplePartitionMap {
public:
	typedef std::function<bool(u64 offset, void* buffer, size_t length)>
		ReadFunction;
	typedef std::function<bool(u64 offset, const void* buffer,
		size_t length)> WriteFunction;

	// The driver descriptor map and the driver and patch partitions of a
	// disk (entries and contents).
	struct Drivers {
		u8					descriptorMap[512];
		struct Partition {
			u8				entry[512];
			u32				start;
			u32				count;
			std::vector<u8>	data;
		};
		std::vector<Partition> partitions;
		u32					end = 0;		// the first block after them
		u32					hfsStatus = 0;	// for the HFS partition
	};

	// The drivers of a disk with 512 byte blocks; false if it has none.
	static	bool				ReadDrivers(DiskImage& disk,
									Drivers& drivers);

	// Block 0 and the partition map of a disk of 'blocks' blocks holding
	// the drivers and an HFS partition named 'name' from 'hfsStart' to the
	// end, and the drivers' contents.
	static	bool				WriteDisk(WriteFunction write,
									const Drivers& drivers, u32 blocks,
									u32 hfsStart, const std::string& name,
									std::string& error);

	// Where the first HFS partition of a disk is (bytes); a disk without a
	// partition map holding an HFS volume is one from 0.
	static	bool				FindHfs(ReadFunction read, u64 diskSize,
									u64& offset, u64& size);
};

}	// namespace powermac

#endif	// PM_APPLE_PARTITION_MAP_H
