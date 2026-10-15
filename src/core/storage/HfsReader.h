/*
 * PowerMac emulator - reads HFS (Mac OS Standard) volumes.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Reads what Mac OS left on a volume HfsWriter made: the catalog (walked
 * through its leaf nodes), the forks with all their extents (the overflow
 * ones from the extents B-tree), and whether the volume was cleanly
 * unmounted.
 */
#ifndef PM_HFS_READER_H
#define PM_HFS_READER_H

#include "../Types.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace powermac {

class HfsReader {
public:
	typedef std::function<bool(u64 offset, void* buffer, size_t length)>
		ReadFunction;

	struct Entry {
		std::string			name;			// Mac OS Roman
		bool				directory = false;
		u32					id = 0;
		u32					created = 0;
		u32					modified = 0;
		u8					finderInfo[32] = {};
		u64					dataSize = 0;	// files
		u64					resourceSize = 0;
	};

	static const u32 kRootId = 2;

								HfsReader(ReadFunction read);

			// The volume at 'offset' bytes; false if there is none.
			bool				Mount(u64 offset);
			bool				IsClean() const { return fClean; }
			const std::string&	VolumeName() const { return fVolumeName; }

			// The folder's items, by name.
			bool				List(u32 directory,
									std::vector<Entry>& entries) const;
			bool				ReadFork(u32 file, bool resource, u64 offset,
									void* buffer, size_t length) const;

private:
	struct Extent {
		u32		start;
		u32		count;
	};
	typedef std::vector<Extent> Extents;

			bool				_ReadFile(const Extents& extents, u64 offset,
									void* buffer, size_t length) const;
			void				_AddExtents(Extents& extents,
									const u8* record) const;
			bool				_LoadExtentsTree(const Extents& extents,
									u64 size);
			Extents				_Overflow(u32 file, bool resource,
									Extents extents, u64 size) const;
			bool				_WalkLeaves(const std::vector<u8>& tree,
									std::function<void(const u8* key,
										const u8* data, size_t length)>
										visit) const;

			ReadFunction		fRead;
			u64					fOffset = 0;
			u32					fBlockSize = 0;
			u64					fFirstBlock = 0;	// bytes
			bool				fClean = false;
			std::string			fVolumeName;
			// the overflow extents: (fork type, file ID, first file block)
			std::map<std::pair<std::pair<u8, u32>, u32>, Extents> fOverflow;
			std::map<u32, std::vector<Entry> > fDirectories;
			std::map<u32, std::pair<Extents, Extents> > fForks;
};

}	// namespace powermac

#endif	// PM_HFS_READER_H
