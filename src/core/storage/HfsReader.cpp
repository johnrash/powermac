/*
 * PowerMac emulator - reads HFS (Mac OS Standard) volumes.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "HfsReader.h"

#include <algorithm>
#include <cstring>

namespace powermac {

namespace {

const u32 kCatalogId = 4;

enum {
	kDirectoryRecord = 1,
	kFileRecord = 2
};

}	// namespace


HfsReader::HfsReader(ReadFunction read)
	:
	fRead(read)
{
}


void
HfsReader::_AddExtents(Extents& extents, const u8* record) const
{
	for (int i = 0; i < 3; i++) {
		Extent extent;
		extent.start = LoadBE16(record + 4 * i);
		extent.count = LoadBE16(record + 4 * i + 2);
		if (extent.count > 0)
			extents.push_back(extent);
	}
}


bool
HfsReader::_ReadFile(const Extents& extents, u64 offset, void* buffer,
	size_t length) const
{
	u8* out = (u8*)buffer;
	for (const Extent& extent : extents) {
		if (length == 0)
			break;
		u64 bytes = (u64)extent.count * fBlockSize;
		if (offset >= bytes) {
			offset -= bytes;
			continue;
		}
		size_t chunk = (size_t)std::min<u64>(length, bytes - offset);
		if (!fRead(fOffset + fFirstBlock + (u64)extent.start * fBlockSize
				+ offset, out, chunk))
			return false;
		out += chunk;
		length -= chunk;
		offset = 0;
	}
	return length == 0;
}


// The fork's extents with those from the overflow file, to cover 'size'.
HfsReader::Extents
HfsReader::_Overflow(u32 file, bool resource, Extents extents, u64 size) const
{
	for (;;) {
		u64 covered = 0;
		for (const Extent& extent : extents)
			covered += extent.count;
		if (covered * fBlockSize >= size)
			break;
		auto found = fOverflow.find(std::make_pair(std::make_pair(
			(u8)(resource ? 0xff : 0), file), (u32)covered));
		if (found == fOverflow.end())
			break;
		extents.insert(extents.end(), found->second.begin(),
			found->second.end());
	}
	return extents;
}


bool
HfsReader::_WalkLeaves(const std::vector<u8>& tree,
	std::function<void(const u8* key, const u8* data, size_t length)> visit)
	const
{
	if (tree.size() < 512)
		return false;
	u32 nodeSize = LoadBE16(&tree[14 + 18]);
	if (nodeSize < 512 || tree.size() < nodeSize)
		return false;
	u32 nodes = (u32)(tree.size() / nodeSize);
	u32 node = LoadBE32(&tree[14 + 10]);
	for (u32 visited = 0; node != 0 && visited < nodes; visited++) {
		if (node >= nodes)
			return false;
		const u8* base = &tree[(size_t)node * nodeSize];
		if (base[8] != 0xff)
			return false;
		u16 records = LoadBE16(base + 10);
		for (u16 i = 0; i < records; i++) {
			u16 start = LoadBE16(base + nodeSize - 2 * (i + 1));
			u16 end = LoadBE16(base + nodeSize - 2 * (i + 2));
			if (start < 14 || end > nodeSize - 2 * (records + 1)
				|| end <= start)
				return false;
			const u8* key = base + start;
			size_t keyBytes = (key[0] + 2) & ~1u;
			if (keyBytes > (size_t)(end - start))
				return false;
			visit(key, key + keyBytes, end - start - keyBytes);
		}
		node = LoadBE32(base);
	}
	return true;
}


bool
HfsReader::_LoadExtentsTree(const Extents& extents, u64 size)
{
	std::vector<u8> tree((size_t)size);
	if (!_ReadFile(extents, 0, tree.data(), tree.size()))
		return false;
	if (LoadBE32(&tree[14 + 2]) == 0)
		return true;			// empty
	return _WalkLeaves(tree, [this](const u8* key, const u8* data,
			size_t length) {
		if (key[0] < 7 || length < 12)
			return;
		Extents& extents = fOverflow[std::make_pair(std::make_pair(key[1],
			LoadBE32(key + 2)), (u32)LoadBE16(key + 6))];
		extents.clear();
		_AddExtents(extents, data);
	});
}


bool
HfsReader::Mount(u64 offset)
{
	fOffset = offset;
	fOverflow.clear();
	fDirectories.clear();
	fForks.clear();
	u8 mdb[512];
	if (!fRead(offset + 1024, mdb, sizeof(mdb)) || LoadBE16(mdb) != 0x4244)
		return false;
	fBlockSize = LoadBE32(mdb + 20);
	if (fBlockSize == 0 || fBlockSize % 512 != 0)
		return false;
	fFirstBlock = (u64)LoadBE16(mdb + 28) * 512;
	fClean = (LoadBE16(mdb + 10) & 0x0100) != 0;
	fVolumeName.assign((const char*)mdb + 37, std::min<u8>(mdb[36], 27));

	Extents extentsFile;
	_AddExtents(extentsFile, mdb + 134);
	u64 extentsSize = LoadBE32(mdb + 130);
	if (extentsSize < 512 || extentsSize > (64 << 20)
		|| !_LoadExtentsTree(extentsFile, extentsSize))
		return false;

	Extents catalogExtents;
	_AddExtents(catalogExtents, mdb + 150);
	u64 catalogSize = LoadBE32(mdb + 146);
	if (catalogSize < 512 || catalogSize > (256 << 20))
		return false;
	catalogExtents = _Overflow(kCatalogId, false, catalogExtents,
		catalogSize);
	std::vector<u8> catalog((size_t)catalogSize);
	if (!_ReadFile(catalogExtents, 0, catalog.data(), catalog.size()))
		return false;
	return _WalkLeaves(catalog, [this](const u8* key, const u8* data,
			size_t length) {
		if (key[0] < 6 || key[6] > 31 || key[6] + 6 > key[0])
			return;
		Entry entry;
		u32 parent = LoadBE32(key + 2);
		entry.name.assign((const char*)key + 7, key[6]);
		if (data[0] == kDirectoryRecord && length >= 70) {
			entry.directory = true;
			entry.id = LoadBE32(data + 6);
			entry.created = LoadBE32(data + 10);
			entry.modified = LoadBE32(data + 14);
			memcpy(entry.finderInfo, data + 22, 32);
		} else if (data[0] == kFileRecord && length >= 102) {
			entry.id = LoadBE32(data + 20);
			entry.dataSize = LoadBE32(data + 26);
			entry.resourceSize = LoadBE32(data + 36);
			entry.created = LoadBE32(data + 44);
			entry.modified = LoadBE32(data + 48);
			memcpy(entry.finderInfo, data + 4, 16);
			memcpy(entry.finderInfo + 16, data + 56, 16);
			Extents dataExtents;
			Extents resourceExtents;
			_AddExtents(dataExtents, data + 74);
			_AddExtents(resourceExtents, data + 86);
			fForks[entry.id] = std::make_pair(
				_Overflow(entry.id, false, dataExtents, entry.dataSize),
				_Overflow(entry.id, true, resourceExtents,
					entry.resourceSize));
		} else
			return;				// threads
		fDirectories[parent].push_back(entry);
	});
}


bool
HfsReader::List(u32 directory, std::vector<Entry>& entries) const
{
	entries.clear();
	auto found = fDirectories.find(directory);
	if (found != fDirectories.end())
		entries = found->second;
	return true;
}


bool
HfsReader::ReadFork(u32 file, bool resource, u64 offset, void* buffer,
	size_t length) const
{
	auto found = fForks.find(file);
	if (found == fForks.end())
		return false;
	return _ReadFile(resource ? found->second.second : found->second.first,
		offset, buffer, length);
}

}	// namespace powermac
