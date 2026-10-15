/*
 * PowerMac emulator - writes HFS (Mac OS Standard) volumes.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Creates an HFS volume holding a tree of folders and files (both forks,
 * Finder information), as Inside Macintosh: Files describes it: boot
 * blocks (empty), the master directory block in sector 2 and a copy in the
 * next to last sector, the volume bitmap from sector 3, then up to 65535
 * allocation blocks holding the extents overflow file, the catalog file
 * and the forks, each fork in one extent. The two B-trees have 512 byte
 * nodes; the catalog has a record and a thread for each folder and a
 * record for each file. The volume is marked as cleanly unmounted.
 */
#ifndef PM_HFS_WRITER_H
#define PM_HFS_WRITER_H

#include "../Types.h"

#include <functional>
#include <string>
#include <vector>

namespace powermac {

class HfsWriter {
public:
	typedef std::function<bool(u64 offset, void* buffer, size_t length)>
		ReadFunction;
	typedef std::function<bool(u64 offset, const void* buffer,
		size_t length)> WriteFunction;

	struct Node {
		std::string			name;			// Mac OS Roman, 1-31 bytes
		bool				directory = false;
		u32					created = 0;	// Mac OS time
		u32					modified = 0;
		u8					finderInfo[32] = {};
							// FInfo or DInfo, then FXInfo or DXInfo
		u64					dataSize = 0;
		ReadFunction		readData;
		u64					resourceSize = 0;
		ReadFunction		readResource;
		std::vector<Node>	children;		// folders

		// set by the writer
		u32					id = 0;			// catalog node ID
	};

	static constexpr u64 kMaxVolumeSize = 0x7fff0000ull;	// 2 GB
	static constexpr u64 kMaxForkSize = 0x7fffffffull;

	// The allocation block size of a volume of 'volumeSize' bytes.
	static	u32					BlockSize(u64 volumeSize);

	// Writes a volume of 'volumeSize' bytes (512 byte sectors from offset
	// 0) holding the children of 'root'; the root's name (up to 27 bytes)
	// names the volume. Children must have distinct names. Fills in each
	// node's ID.
	static	bool				Write(WriteFunction write, u64 volumeSize,
									Node& root, std::string& error);
};

}	// namespace powermac

#endif	// PM_HFS_WRITER_H
