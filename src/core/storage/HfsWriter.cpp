/*
 * PowerMac emulator - writes HFS (Mac OS Standard) volumes.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "HfsWriter.h"

#include "MacRoman.h"

#include <algorithm>
#include <cstring>

namespace powermac {

namespace {

const u32 kNodeSize = 512;
const u32 kRootParentId = 1;
const u32 kRootId = 2;
const u32 kFirstUserId = 16;
const u8 kCatalogKeyLength = 37;
const u8 kExtentsKeyLength = 7;

enum {
	kDirectoryRecord = 1,
	kFileRecord = 2,
	kDirectoryThread = 3
};

enum {
	kIndexNode = 0,
	kHeaderNode = 1,
	kMapNode = 2,
	kLeafNode = 0xff
};

// node bits in the header node's map record and in a map node's (which
// leaves two bytes free before the offsets)
const u32 kHeaderMapNodes = 256 * 8;
const u32 kMapNodeNodes = 492 * 8;

const u32 kFileRecordSize = 102;
const u32 kDirectoryRecordSize = 70;
const u32 kThreadRecordSize = 46;

struct Record {
	u32						parent;
	std::string				name;
	std::vector<u8>			data;
	HfsWriter::Node*		file = nullptr;
};


// A catalog key: in a leaf its length, padded to even; in an index node
// always the longest (HFS index keys are not variable).
std::vector<u8>
CatalogKey(u32 parent, const std::string& name, bool index)
{
	std::vector<u8> key(index ? kCatalogKeyLength + 1
		: (7 + name.size() + 1) & ~(size_t)1);
	key[0] = index ? kCatalogKeyLength : (u8)(6 + name.size());
	StoreBE32(&key[2], parent);
	key[6] = (u8)name.size();
	memcpy(&key[7], name.data(), name.size());
	return key;
}


bool
ValidName(const std::string& name, size_t maxLength)
{
	return !name.empty() && name.size() <= maxLength
		&& name.find(':') == std::string::npos;
}


struct Layout {
	u32		sectors;
	u32		blockSize;
	u32		blocks;
	u32		bitmapSectors;
	u32		firstBlockSector;	// drAlBlSt
};


Layout
ComputeLayout(u64 volumeSize)
{
	Layout layout;
	layout.sectors = (u32)(volumeSize / 512);
	u32 blockSectors = std::max<u32>(1,
		(layout.sectors - 21 + 65534) / 65535);
	layout.blockSize = blockSectors * 512;
	u32 estimate = std::min<u32>(65535, (layout.sectors - 5) / blockSectors);
	layout.bitmapSectors = (estimate + 4095) / 4096;
	layout.blocks = std::min<u32>(65535,
		(layout.sectors - 5 - layout.bitmapSectors) / blockSectors);
	layout.firstBlockSector = 3 + layout.bitmapSectors;
	return layout;
}


class Builder {
public:
	Builder(HfsWriter::WriteFunction write, u64 volumeSize,
			HfsWriter::Node& root, std::string& error)
		:
		fWrite(write),
		fVolumeSize(volumeSize),
		fRoot(root),
		fError(error)
	{
	}

	bool Run();

private:
	bool _Collect(HfsWriter::Node& directory, u32 parent);
	void _PackLeaves();
	void _PackIndex();
	bool _Allocate(u64 bytes, u16& start, u16& count);
	bool _WriteCatalog();
	bool _WriteExtentsFile();
	bool _WriteBitmap();
	bool _WriteForks();
	bool _WriteFork(const HfsWriter::ReadFunction& read, u64 size,
		u16 start);
	bool _WriteMdb();
	void _NodeHeader(u8* node, u32 forward, u32 backward, u8 type,
		u8 height, u16 records);
	void _HeaderNode(u8* node, u32 depth, u32 root, u32 leafRecords,
		u32 firstLeaf, u32 lastLeaf, u16 maxKeyLength, u32 totalNodes,
		u32 usedNodes, u32 firstMap);
	void _MarkNodes(std::vector<u8>& image, u32 usedNodes, u32 mapNodes,
		u32 firstMap);
	u64 _BlockOffset(u32 block) const
		{ return (u64)fLayout.firstBlockSector * 512
			+ (u64)block * fLayout.blockSize; }

	HfsWriter::WriteFunction fWrite;
	u64					fVolumeSize;
	HfsWriter::Node&	fRoot;
	std::string&		fError;

	Layout				fLayout;
	u32					fNextId = kFirstUserId;
	u32					fFiles = 0;			// all but the root
	u32					fDirectories = 0;
	u32					fRootFiles = 0;
	u32					fRootDirectories = 0;
	std::vector<Record>	fRecords;
	std::vector<HfsWriter::Node*> fFileNodes;

	// the catalog: its leaves (record indices) and index levels (child
	// node numbers, from the leaves up)
	std::vector<std::vector<size_t> > fLeaves;
	std::vector<std::vector<std::vector<u32> > > fIndex;
	u32					fMapNodes = 0;
	u32					fCatalogNodes = 0;
	u32					fUsedCatalogNodes = 0;
	u32					fFirstLeaf = 0;

	u32					fNextBlock = 0;
	u16					fExtentsStart = 0;
	u16					fExtentsBlocks = 0;
	u16					fCatalogStart = 0;
	u16					fCatalogBlocks = 0;
};


bool
Builder::_Collect(HfsWriter::Node& directory, u32 parent)
{
	std::sort(directory.children.begin(), directory.children.end(),
		[](const HfsWriter::Node& a, const HfsWriter::Node& b) {
			return CompareHfsNames(a.name, b.name) < 0;
		});
	for (size_t i = 0; i < directory.children.size(); i++) {
		HfsWriter::Node& child = directory.children[i];
		if (!ValidName(child.name, 31)) {
			fError = "the name \"" + child.name + "\" does not suit HFS";
			return false;
		}
		if (i > 0 && CompareHfsNames(directory.children[i - 1].name,
				child.name) == 0) {
			fError = "two items named \"" + child.name + "\" in one folder";
			return false;
		}
		if (!child.directory && (child.dataSize > HfsWriter::kMaxForkSize
				|| child.resourceSize > HfsWriter::kMaxForkSize)) {
			fError = "the file \"" + child.name + "\" is too big for HFS";
			return false;
		}
		child.id = fNextId++;
	}

	for (HfsWriter::Node& child : directory.children) {
		Record record;
		record.parent = parent;
		record.name = child.name;
		if (child.directory) {
			fDirectories++;
			if (parent == kRootId)
				fRootDirectories++;
			record.data.resize(kDirectoryRecordSize);
			u8* data = record.data.data();
			data[0] = kDirectoryRecord;
			StoreBE16(data + 4, (u16)std::min<size_t>(65535,
				child.children.size()));
			StoreBE32(data + 6, child.id);
			StoreBE32(data + 10, child.created);
			StoreBE32(data + 14, child.modified);
			memcpy(data + 22, child.finderInfo, 32);
			fRecords.push_back(record);

			Record thread;
			thread.parent = child.id;
			thread.data.resize(kThreadRecordSize);
			thread.data[0] = kDirectoryThread;
			StoreBE32(&thread.data[10], parent);
			thread.data[14] = (u8)child.name.size();
			memcpy(&thread.data[15], child.name.data(), child.name.size());
			fRecords.push_back(thread);
			if (!_Collect(child, child.id))
				return false;
		} else {
			fFiles++;
			if (parent == kRootId)
				fRootFiles++;
			record.data.resize(kFileRecordSize);
			u8* data = record.data.data();
			data[0] = kFileRecord;
			data[2] = 0x80;			// the record is in use
			memcpy(data + 4, child.finderInfo, 16);
			StoreBE32(data + 20, child.id);
			StoreBE32(data + 26, (u32)child.dataSize);
			StoreBE32(data + 36, (u32)child.resourceSize);
			StoreBE32(data + 44, child.created);
			StoreBE32(data + 48, child.modified);
			memcpy(data + 56, child.finderInfo + 16, 16);
			record.file = &child;
			fRecords.push_back(record);
			fFileNodes.push_back(&child);
		}
	}
	return true;
}


void
Builder::_PackLeaves()
{
	std::sort(fRecords.begin(), fRecords.end(),
		[](const Record& a, const Record& b) {
			if (a.parent != b.parent)
				return a.parent < b.parent;
			return CompareHfsNames(a.name, b.name) < 0;
		});
	// a node: 14 bytes of descriptor, the records, an offset for each and
	// one for the free space
	std::vector<size_t> leaf;
	u32 used = 14 + 2;
	for (size_t i = 0; i < fRecords.size(); i++) {
		u32 size = (u32)(CatalogKey(fRecords[i].parent, fRecords[i].name,
			false).size() + fRecords[i].data.size()) + 2;
		if (used + size > kNodeSize) {
			fLeaves.push_back(leaf);
			leaf.clear();
			used = 14 + 2;
		}
		leaf.push_back(i);
		used += size;
	}
	if (!leaf.empty())
		fLeaves.push_back(leaf);
}


void
Builder::_PackIndex()
{
	const u32 perNode = (kNodeSize - 14 - 2) / (kCatalogKeyLength + 1 + 4 + 2);
	// node numbers: the header, the map nodes, the leaves, then the index
	// nodes level by level
	u32 count = (u32)fLeaves.size();
	fIndex.clear();
	while (count > 1) {
		std::vector<std::vector<u32> > level;
		for (u32 i = 0; i < count; i += perNode) {
			std::vector<u32> children;
			for (u32 j = i; j < std::min(count, i + perNode); j++)
				children.push_back(j);		// relative to the level below
			level.push_back(children);
		}
		count = (u32)level.size();
		fIndex.push_back(level);
	}
}


bool
Builder::_Allocate(u64 bytes, u16& start, u16& count)
{
	u64 blocks = (bytes + fLayout.blockSize - 1) / fLayout.blockSize;
	if (fNextBlock + blocks > fLayout.blocks) {
		fError = "the files do not fit the volume";
		return false;
	}
	start = blocks == 0 ? 0 : (u16)fNextBlock;
	count = (u16)blocks;
	fNextBlock += (u32)blocks;
	return true;
}


void
Builder::_NodeHeader(u8* node, u32 forward, u32 backward, u8 type,
	u8 height, u16 records)
{
	StoreBE32(node, forward);
	StoreBE32(node + 4, backward);
	node[8] = type;
	node[9] = height;
	StoreBE16(node + 10, records);
}


void
Builder::_HeaderNode(u8* node, u32 depth, u32 root, u32 leafRecords,
	u32 firstLeaf, u32 lastLeaf, u16 maxKeyLength, u32 totalNodes,
	u32 usedNodes, u32 firstMap)
{
	_NodeHeader(node, firstMap, 0, kHeaderNode, 0, 3);
	u8* header = node + 14;
	StoreBE16(header, (u16)depth);
	StoreBE32(header + 2, root);
	StoreBE32(header + 6, leafRecords);
	StoreBE32(header + 10, firstLeaf);
	StoreBE32(header + 14, lastLeaf);
	StoreBE16(header + 18, kNodeSize);
	StoreBE16(header + 20, maxKeyLength);
	StoreBE32(header + 22, totalNodes);
	StoreBE32(header + 26, totalNodes - usedNodes);
	// the header record (106 bytes), the user record (128), the map record
	// (256) and the free space
	StoreBE16(node + 510, 14);
	StoreBE16(node + 508, 120);
	StoreBE16(node + 506, 248);
	StoreBE16(node + 504, 504);
}


// Marks nodes 0 .. usedNodes - 1 in use in the header's map record and the
// map nodes (which follow it from 'firstMap').
void
Builder::_MarkNodes(std::vector<u8>& image, u32 usedNodes, u32 mapNodes,
	u32 firstMap)
{
	for (u32 i = 0; i < mapNodes; i++) {
		u8* node = &image[(firstMap + i) * kNodeSize];
		_NodeHeader(node, i + 1 < mapNodes ? firstMap + i + 1 : 0, 0,
			kMapNode, 0, 1);
		StoreBE16(node + 510, 14);
		StoreBE16(node + 508, 14 + 492);
	}
	for (u32 n = 0; n < usedNodes; n++) {
		u8* bits;
		u32 bit;
		if (n < kHeaderMapNodes) {
			bits = &image[248];
			bit = n;
		} else {
			u32 map = (n - kHeaderMapNodes) / kMapNodeNodes;
			bits = &image[(firstMap + map) * kNodeSize + 14];
			bit = (n - kHeaderMapNodes) % kMapNodeNodes;
		}
		bits[bit / 8] |= 0x80 >> (bit % 8);
	}
}


bool
Builder::_WriteCatalog()
{
	std::vector<u8> image((size_t)fCatalogNodes * kNodeSize);
	u32 firstMap = 1;
	fFirstLeaf = 1 + fMapNodes;
	u32 leafCount = (u32)fLeaves.size();

	// leaves
	for (u32 i = 0; i < leafCount; i++) {
		u8* node = &image[(fFirstLeaf + i) * kNodeSize];
		_NodeHeader(node, i + 1 < leafCount ? fFirstLeaf + i + 1 : 0,
			i > 0 ? fFirstLeaf + i - 1 : 0, kLeafNode, 1,
			(u16)fLeaves[i].size());
		u32 offset = 14;
		for (size_t r = 0; r < fLeaves[i].size(); r++) {
			const Record& record = fRecords[fLeaves[i][r]];
			std::vector<u8> key = CatalogKey(record.parent, record.name,
				false);
			StoreBE16(node + kNodeSize - 2 * (r + 1), (u16)offset);
			memcpy(node + offset, key.data(), key.size());
			offset += (u32)key.size();
			memcpy(node + offset, record.data.data(), record.data.size());
			offset += (u32)record.data.size();
		}
		StoreBE16(node + kNodeSize - 2 * (fLeaves[i].size() + 1),
			(u16)offset);
	}

	// index levels: the first key of each child node
	std::vector<u32> belowNodes;
	std::vector<std::pair<u32, std::string> > belowKeys;
	for (u32 i = 0; i < leafCount; i++) {
		belowNodes.push_back(fFirstLeaf + i);
		const Record& first = fRecords[fLeaves[i][0]];
		belowKeys.push_back(std::make_pair(first.parent, first.name));
	}
	u32 nextNode = fFirstLeaf + leafCount;
	u32 root = fFirstLeaf;
	for (size_t level = 0; level < fIndex.size(); level++) {
		std::vector<u32> nodes;
		std::vector<std::pair<u32, std::string> > keys;
		const auto& groups = fIndex[level];
		for (size_t g = 0; g < groups.size(); g++) {
			u32 number = nextNode++;
			u8* node = &image[number * kNodeSize];
			_NodeHeader(node, g + 1 < groups.size() ? number + 1 : 0,
				g > 0 ? number - 1 : 0, kIndexNode, (u8)(level + 2),
				(u16)groups[g].size());
			u32 offset = 14;
			for (size_t r = 0; r < groups[g].size(); r++) {
				u32 child = groups[g][r];
				std::vector<u8> key = CatalogKey(belowKeys[child].first,
					belowKeys[child].second, true);
				StoreBE16(node + kNodeSize - 2 * (r + 1), (u16)offset);
				memcpy(node + offset, key.data(), key.size());
				offset += (u32)key.size();
				StoreBE32(node + offset, belowNodes[child]);
				offset += 4;
			}
			StoreBE16(node + kNodeSize - 2 * (groups[g].size() + 1),
				(u16)offset);
			nodes.push_back(number);
			keys.push_back(belowKeys[groups[g][0]]);
		}
		belowNodes = nodes;
		belowKeys = keys;
		root = nodes.back();
	}

	_HeaderNode(&image[0], (u32)fIndex.size() + 1, root,
		(u32)fRecords.size(), fFirstLeaf, fFirstLeaf + leafCount - 1,
		kCatalogKeyLength, fCatalogNodes, fUsedCatalogNodes,
		fMapNodes > 0 ? firstMap : 0);
	_MarkNodes(image, fUsedCatalogNodes, fMapNodes, firstMap);

	if (!fWrite(_BlockOffset(fCatalogStart), image.data(), image.size())) {
		fError = "cannot write the catalog";
		return false;
	}
	return true;
}


bool
Builder::_WriteExtentsFile()
{
	u32 nodes = fExtentsBlocks * fLayout.blockSize / kNodeSize;
	std::vector<u8> header(kNodeSize);
	_HeaderNode(header.data(), 0, 0, 0, 0, 0, kExtentsKeyLength, nodes, 1,
		0);
	header[248] = 0x80;
	if (!fWrite(_BlockOffset(fExtentsStart), header.data(), header.size())) {
		fError = "cannot write the extents file";
		return false;
	}
	return true;
}


bool
Builder::_WriteBitmap()
{
	std::vector<u8> bitmap((size_t)fLayout.bitmapSectors * 512);
	for (u32 block = 0; block < fNextBlock; block++)
		bitmap[block / 8] |= 0x80 >> (block % 8);
	if (!fWrite(3 * 512, bitmap.data(), bitmap.size())) {
		fError = "cannot write the volume bitmap";
		return false;
	}
	return true;
}


bool
Builder::_WriteFork(const HfsWriter::ReadFunction& read, u64 size,
	u16 start)
{
	std::vector<u8> buffer(1 << 20);
	for (u64 offset = 0; offset < size; ) {
		size_t length = (size_t)std::min<u64>(buffer.size(), size - offset);
		// a file that cannot be read keeps zeros
		if (!read || !read(offset, buffer.data(), length))
			memset(buffer.data(), 0, length);
		if (!fWrite(_BlockOffset(start) + offset, buffer.data(), length)) {
			fError = "cannot write the volume";
			return false;
		}
		offset += length;
	}
	return true;
}


bool
Builder::_WriteForks()
{
	for (const Record& record : fRecords) {
		if (record.file == nullptr)
			continue;
		const u8* data = record.data.data();
		const HfsWriter::Node& file = *record.file;
		if (!_WriteFork(file.readData, file.dataSize, LoadBE16(data + 74))
			|| !_WriteFork(file.readResource, file.resourceSize,
				LoadBE16(data + 86)))
			return false;
	}
	return true;
}


bool
Builder::_WriteMdb()
{
	u8 mdb[512] = {};
	StoreBE16(mdb, 0x4244);				// 'BD'
	StoreBE32(mdb + 2, fRoot.created);
	StoreBE32(mdb + 6, fRoot.modified);
	StoreBE16(mdb + 10, 0x0100);		// unmounted cleanly
	StoreBE16(mdb + 12, (u16)std::min<u32>(65535, fRootFiles));
	StoreBE16(mdb + 14, 3);				// the bitmap
	StoreBE16(mdb + 16, (u16)std::min<u32>(65535, fNextBlock));
	StoreBE16(mdb + 18, (u16)fLayout.blocks);
	StoreBE32(mdb + 20, fLayout.blockSize);
	StoreBE32(mdb + 24, 4 * fLayout.blockSize);
	StoreBE16(mdb + 28, (u16)fLayout.firstBlockSector);
	StoreBE32(mdb + 30, fNextId);
	StoreBE16(mdb + 34, (u16)(fLayout.blocks - fNextBlock));
	mdb[36] = (u8)fRoot.name.size();
	memcpy(mdb + 37, fRoot.name.data(), fRoot.name.size());
	StoreBE32(mdb + 74, fExtentsBlocks * fLayout.blockSize);
	StoreBE32(mdb + 78, fCatalogBlocks * fLayout.blockSize);
	StoreBE16(mdb + 82, (u16)std::min<u32>(65535, fRootDirectories));
	StoreBE32(mdb + 84, fFiles);
	StoreBE32(mdb + 88, fDirectories);
	StoreBE32(mdb + 130, fExtentsBlocks * fLayout.blockSize);
	StoreBE16(mdb + 134, fExtentsStart);
	StoreBE16(mdb + 136, fExtentsBlocks);
	StoreBE32(mdb + 146, fCatalogBlocks * fLayout.blockSize);
	StoreBE16(mdb + 150, fCatalogStart);
	StoreBE16(mdb + 152, fCatalogBlocks);
	if (!fWrite(2 * 512, mdb, sizeof(mdb))
		|| !fWrite((u64)(fLayout.sectors - 2) * 512, mdb, sizeof(mdb))) {
		fError = "cannot write the master directory block";
		return false;
	}
	return true;
}


bool
Builder::Run()
{
	if (!ValidName(fRoot.name, 27)) {
		fError = "the volume name \"" + fRoot.name + "\" does not suit HFS";
		return false;
	}
	if (fVolumeSize < 400 * 1024 || fVolumeSize > HfsWriter::kMaxVolumeSize) {
		fError = "an HFS volume is from 400 KB to 2 GB";
		return false;
	}
	fLayout = ComputeLayout(fVolumeSize);
	fRoot.id = kRootId;

	// the root: its record (named after the volume) and its thread
	Record rootRecord;
	rootRecord.parent = kRootParentId;
	rootRecord.name = fRoot.name;
	rootRecord.data.resize(kDirectoryRecordSize);
	rootRecord.data[0] = kDirectoryRecord;
	StoreBE16(&rootRecord.data[4], (u16)std::min<size_t>(65535,
		fRoot.children.size()));
	StoreBE32(&rootRecord.data[6], kRootId);
	StoreBE32(&rootRecord.data[10], fRoot.created);
	StoreBE32(&rootRecord.data[14], fRoot.modified);
	memcpy(&rootRecord.data[22], fRoot.finderInfo, 32);
	fRecords.push_back(rootRecord);
	Record rootThread;
	rootThread.parent = kRootId;
	rootThread.data.resize(kThreadRecordSize);
	rootThread.data[0] = kDirectoryThread;
	StoreBE32(&rootThread.data[10], kRootParentId);
	rootThread.data[14] = (u8)fRoot.name.size();
	memcpy(&rootThread.data[15], fRoot.name.data(), fRoot.name.size());
	fRecords.push_back(rootThread);
	if (!_Collect(fRoot, kRootId))
		return false;

	_PackLeaves();
	_PackIndex();
	u32 indexNodes = 0;
	for (const auto& level : fIndex)
		indexNodes += (u32)level.size();
	// room for Mac OS to add to the catalog: half as much again, at least
	// 1024 nodes (a sixteenth of a small volume), in whole allocation
	// blocks
	u32 nodesPerBlock = fLayout.blockSize / kNodeSize;
	u32 minimumFree = std::min<u32>(1024, fLayout.sectors / 16);
	fMapNodes = 0;
	for (int i = 0; i < 8; i++) {
		fUsedCatalogNodes = 1 + fMapNodes + (u32)fLeaves.size() + indexNodes;
		u32 total = fUsedCatalogNodes
			+ std::max<u32>(fUsedCatalogNodes / 2, minimumFree);
		total = (total + nodesPerBlock - 1) / nodesPerBlock * nodesPerBlock;
		u32 mapNodes = total > kHeaderMapNodes
			? (total - kHeaderMapNodes + kMapNodeNodes - 1) / kMapNodeNodes
			: 0;
		fCatalogNodes = total;
		if (mapNodes == fMapNodes)
			break;
		fMapNodes = mapNodes;
	}

	u64 extentsBytes = std::max<u64>(fLayout.blockSize,
		std::min<u64>(256 * 1024, fVolumeSize / 128));
	if (!_Allocate(extentsBytes, fExtentsStart, fExtentsBlocks)
		|| !_Allocate((u64)fCatalogNodes * kNodeSize, fCatalogStart,
			fCatalogBlocks))
		return false;

	// the forks, in catalog order (a folder's files together)
	for (Record& record : fRecords) {
		if (record.file == nullptr)
			continue;
		u8* data = record.data.data();
		u16 start;
		u16 count;
		if (!_Allocate(record.file->dataSize, start, count))
			return false;
		StoreBE16(data + 24, start);
		StoreBE32(data + 30, count * fLayout.blockSize);
		StoreBE16(data + 74, start);
		StoreBE16(data + 76, count);
		if (!_Allocate(record.file->resourceSize, start, count))
			return false;
		StoreBE16(data + 34, start);
		StoreBE32(data + 40, count * fLayout.blockSize);
		StoreBE16(data + 86, start);
		StoreBE16(data + 88, count);
	}

	return _WriteCatalog() && _WriteExtentsFile() && _WriteBitmap()
		&& _WriteForks() && _WriteMdb();
}

}	// namespace


u32
HfsWriter::BlockSize(u64 volumeSize)
{
	return ComputeLayout(volumeSize).blockSize;
}


bool
HfsWriter::Write(WriteFunction write, u64 volumeSize, Node& root,
	std::string& error)
{
	Builder builder(write, volumeSize, root, error);
	return builder.Run();
}

}	// namespace powermac
