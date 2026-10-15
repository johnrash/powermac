/*
 * PowerMac emulator - floppy disk images.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "FloppyImage.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace powermac {

namespace {

const u64 kDiskCopyHeader = 84;

bool
KindOfSize(u64 size, FloppyImage::Kind& kind)
{
	switch (size) {
		case 409600: kind = FloppyImage::kGcr400; return true;
		case 819200: kind = FloppyImage::kGcr800; return true;
		case 737280: kind = FloppyImage::kMfm720; return true;
		case 1474560: kind = FloppyImage::kMfm1440; return true;
	}
	return false;
}


// The sectors of the volume at the start of a raw image (0 if unknown):
// HFS and MFS say how many allocation blocks they have, FAT how many
// sectors.
u32
VolumeSectors(DiskImage& image)
{
	u8 block[512];
	if (image.Read(1024, block, sizeof(block))) {
		u16 signature = LoadBE16(block);
		if (signature == 0x4244 || signature == 0xd2d7) {
			u32 blocks = LoadBE16(block + 18);
			u32 blockSize = LoadBE32(block + 20);
			u32 first = LoadBE16(block + 28);
			if (blockSize >= 512 && blockSize % 512 == 0)
				return first + blocks * (blockSize / 512) + 2;
		}
	}
	if (image.Read(0, block, sizeof(block)) && block[510] == 0x55
		&& block[511] == 0xaa && LoadLE16(block + 11) == 512)
		return LoadLE16(block + 19);
	return 0;
}

// A disk image kept in memory (a compressed one put back together); read
// only.
class MemoryDisk : public DiskImage {
public:
	MemoryDisk(std::vector<u8> data, const std::string& path)
		: fData(std::move(data)), fPath(path) {}

	virtual u64 Size() const { return fData.size(); }
	virtual bool IsReadOnly() const { return true; }
	virtual bool Read(u64 offset, void* buffer, size_t length)
	{
		if (offset + length > fData.size())
			return false;
		memcpy(buffer, fData.data() + offset, length);
		return true;
	}
	virtual bool Write(u64, const void*, size_t) { return false; }
	virtual std::string Path() const { return fPath; }
	virtual const char* Format() const { return "Disk Copy 6"; }

private:
	std::vector<u8> fData;
	std::string fPath;
};


// One chunk of Apple Data Compression from 'position', 'size' bytes of
// it: runs of bytes as they are (0x80 + length - 1), and copies of what
// came before (two bytes: length 3-18 and a distance up to 1024; three:
// length 4-67 and a distance up to 65536). False if it is none.
bool
AdcChunk(const std::vector<u8>& source, size_t& position, size_t size,
	std::vector<u8>& out)
{
	size_t start = out.size();
	size_t end = start + size;
	while (out.size() < end) {
		if (position >= source.size())
			return false;
		u8 code = source[position];
		size_t length;
		size_t distance;
		if ((code & 0x80) != 0) {
			length = (code & 0x7f) + 1;
			if (position + 1 + length > source.size())
				return false;
			out.insert(out.end(), source.begin() + position + 1,
				source.begin() + position + 1 + length);
			position += 1 + length;
			continue;
		}
		if ((code & 0x40) != 0) {
			if (position + 3 > source.size())
				return false;
			length = (code & 0x3f) + 4;
			distance = (source[position + 1] << 8 | source[position + 2]) + 1;
			position += 3;
		} else {
			if (position + 2 > source.size())
				return false;
			length = ((code >> 2) & 0x0f) + 3;
			distance = ((code & 3) << 8 | source[position + 1]) + 1;
			position += 2;
		}
		if (distance > out.size() - start)
			return false;
		for (size_t i = 0; i < length; i++)
			out.push_back(out[out.size() - distance]);
	}
	return out.size() == end;
}


// The chunks of the used blocks from 'position', each 'chunkSize' bytes
// (the last one less) compressed or stored, up to the last sector's 512
// bytes at the end of the file. The number of ways that fit (up to 2),
// the first one's data in 'out'.
int
SplitChunks(const std::vector<u8>& source, size_t position, size_t remaining,
	size_t chunkSize, std::vector<u8>& out)
{
	if (remaining == 0)
		return source.size() - position == 512 ? 1 : 0;
	size_t size = std::min(remaining, chunkSize);
	size_t mark = out.size();
	int ways = 0;
	size_t next = position;
	if (AdcChunk(source, next, size, out)) {
		ways = SplitChunks(source, next, remaining - size, chunkSize, out);
		if (ways == 0)
			out.resize(mark);
	} else
		out.resize(mark);
	if (ways < 2 && position + size <= source.size()) {
		std::vector<u8> other;
		std::vector<u8>& target = ways == 0 ? out : other;
		target.insert(target.end(), source.begin() + position,
			source.begin() + position + size);
		int stored = SplitChunks(source, position + size, remaining - size,
			chunkSize, target);
		if (stored == 0 && ways == 0)
			out.resize(mark);
		ways += stored;
	}
	return std::min(ways, 2);
}


// A compressed Disk Copy 6 image (NDIF) whose resource fork, the map of
// its chunks, is lost, put back together if it can be without it: the
// sectors before the HFS allocation blocks are stored as they are, then
// the used blocks (one run from the first, as on a disk Disk Copy wrote)
// in chunks of 512 sectors, each ADC compressed or stored, then the next
// to last sector (the spare master directory block) as it is; free blocks
// are left out. It must fit one way only.
bool
RebuildDiskCopy6(DiskImage& image, std::vector<u8>& disk)
{
	if (image.Size() > 2 * 1474560)
		return false;
	std::vector<u8> source((size_t)image.Size());
	if (!image.Read(0, source.data(), source.size()) || source.size() < 2048)
		return false;
	const u8* mdb = &source[1024];
	if (LoadBE16(mdb) != 0x4244)
		return false;
	u32 blocks = LoadBE16(mdb + 18);
	u32 blockSize = LoadBE32(mdb + 20);
	u32 first = LoadBE16(mdb + 28);
	u32 bitmapSector = LoadBE16(mdb + 14);
	if (blockSize == 0 || blockSize % 512 != 0 || first < 3
		|| bitmapSector >= first || (size_t)first * 512 > source.size())
		return false;
	u32 sectors = first + blocks * (blockSize / 512) + 2;
	FloppyImage::Kind kind;
	if (!KindOfSize((u64)sectors * 512, kind))
		return false;
	// the used blocks: one run from block 0
	const u8* bitmap = &source[bitmapSector * 512];
	size_t bitmapBytes = (size_t)(first - bitmapSector) * 512;
	auto used = [&](u32 block) {
		return block / 8 < bitmapBytes
			&& (bitmap[block / 8] & (0x80 >> (block % 8))) != 0;
	};
	u32 run = 0;
	while (run < blocks && used(run))
		run++;
	for (u32 block = run; block < blocks; block++) {
		if (used(block))
			return false;
	}
	std::vector<u8> data;
	if (SplitChunks(source, (size_t)first * 512, (size_t)run * blockSize,
			512 * 512, data) != 1)
		return false;
	disk.assign((size_t)sectors * 512, 0);
	memcpy(disk.data(), source.data(), (size_t)first * 512);
	memcpy(disk.data() + (size_t)first * 512, data.data(), data.size());
	memcpy(disk.data() + (size_t)(sectors - 2) * 512,
		source.data() + source.size() - 512, 512);
	return true;
}


// A compressed Disk Copy 6 image (NDIF) without its resource fork, which
// holds the map of the chunks the data fork has: the first chunk (the
// boot blocks and the HFS master directory block) as it is, then the
// others compressed (ADC). Its size is no number of sectors, and the
// extents B-tree is not where the master directory block says.
bool
IsCompressedDiskCopy(DiskImage& image)
{
	if (image.Size() % 512 != 0)
		return true;
	u8 block[512];
	if (!image.Read(1024, block, sizeof(block)) || LoadBE16(block) != 0x4244)
		return false;
	u32 blockSectors = LoadBE32(block + 20) / 512;
	u64 extents = ((u64)LoadBE16(block + 28)
		+ (u64)LoadBE16(block + 134) * blockSectors) * 512;
	if (blockSectors == 0 || extents + 512 > image.Size()
		|| !image.Read(extents, block, sizeof(block)))
		return false;
	// a header node: type 1, 3 records, 512 byte nodes
	return block[8] != 1 || LoadBE16(block + 10) != 3
		|| LoadBE16(block + 32) != 512;
}

}	// namespace


FloppyImage::FloppyImage(std::unique_ptr<DiskImage> image, Kind kind,
	u64 dataOffset, u64 tagSize)
	:
	fImage(std::move(image)),
	fKind(kind),
	fSize(KindSize(kind)),
	fDataOffset(dataOffset),
	fTagSize(tagSize),
	fDirty(false)
{
}


FloppyImage::~FloppyImage()
{
	Flush();
}


u64
FloppyImage::KindSize(Kind kind)
{
	switch (kind) {
		case kGcr400: return 409600;
		case kGcr800: return 819200;
		case kMfm720: return 737280;
		case kMfm1440: return 1474560;
	}
	return 0;
}


std::unique_ptr<FloppyImage>
FloppyImage::Wrap(std::unique_ptr<DiskImage> image, std::string& error)
{
	u64 size = image->Size();
	Kind kind;

	// Disk Copy 4.2: the header's sizes, and its private word 0x0100
	u8 header[kDiskCopyHeader];
	if (size > kDiskCopyHeader
		&& image->Read(0, header, sizeof(header)) && header[0] < 64
		&& header[82] == 0x01 && header[83] == 0x00) {
		u32 dataSize = LoadBE32(header + 64);
		u32 tagSize = LoadBE32(header + 68);
		if (KindOfSize(dataSize, kind)
			&& size >= kDiskCopyHeader + dataSize) {
			if (tagSize != dataSize / 512 * kTagSize
				|| size < kDiskCopyHeader + dataSize + tagSize)
				tagSize = 0;
			return std::unique_ptr<FloppyImage>(new FloppyImage(
				std::move(image), kind, kDiskCopyHeader, tagSize));
		}
	}

	if (size == 0 || size > 1474560) {
		error = "not a floppy disk image (400 KB, 800 KB, 720 KB, 1.44 MB "
			"or Disk Copy 4.2)";
		return nullptr;
	}
	if (IsCompressedDiskCopy(*image)) {
		std::vector<u8> disk;
		if (RebuildDiskCopy6(*image, disk)) {
			std::string path = image->Path();
			KindOfSize(disk.size(), kind);
			return std::unique_ptr<FloppyImage>(new FloppyImage(
				std::unique_ptr<DiskImage>(new MemoryDisk(std::move(disk),
					path)), kind, 0, 0));
		}
		error = "a compressed Disk Copy 6 image (NDIF): the map of its "
			"compressed data is in the file's resource fork, which a disk "
			"drive does not read. Mount it with Disk Copy in Mac OS (from a "
			"Shared Mac folder, with its resource fork), or convert it to a "
			"Disk Copy 4.2 image there";
		return nullptr;
	}
	if (!KindOfSize(size, kind)) {
		// cut short: the volume tells
		u32 sectors = VolumeSectors(*image);
		if (sectors == 1440)
			kind = kMfm720;
		else if (sectors >= size / 512 && sectors <= 800)
			kind = kGcr400;
		else if (sectors >= size / 512 && sectors <= 1600)
			kind = kGcr800;
		else if (sectors >= size / 512 && sectors <= 2880)
			kind = kMfm1440;
		else
			kind = size <= 409600 ? kGcr400 : size <= 819200 ? kGcr800
				: kMfm1440;
	}
	return std::unique_ptr<FloppyImage>(new FloppyImage(std::move(image),
		kind, 0, 0));
}


bool
FloppyImage::Read(u64 offset, void* buffer, size_t length)
{
	if (offset + length > fSize)
		return false;
	// a file cut short reads as zeros past its end
	u64 stored = fImage->Size() > fDataOffset
		? fImage->Size() - fDataOffset : 0;
	size_t inFile = offset >= stored ? 0
		: (size_t)std::min<u64>(length, stored - offset);
	memset((u8*)buffer + inFile, 0, length - inFile);
	return inFile == 0
		|| fImage->Read(fDataOffset + offset, buffer, inFile);
}


bool
FloppyImage::Write(u64 offset, const void* buffer, size_t length)
{
	if (offset + length > fSize)
		return false;
	fDirty = true;
	return fImage->Write(fDataOffset + offset, buffer, length);
}


bool
FloppyImage::ReadTag(u32 sector, u8 tag[kTagSize])
{
	if (fTagSize == 0 || (u64)(sector + 1) * kTagSize > fTagSize) {
		memset(tag, 0, kTagSize);
		return true;
	}
	return fImage->Read(fDataOffset + fSize + (u64)sector * kTagSize, tag,
		kTagSize);
}


bool
FloppyImage::WriteTag(u32 sector, const u8 tag[kTagSize])
{
	if (fTagSize == 0 || (u64)(sector + 1) * kTagSize > fTagSize)
		return true;		// nowhere to keep them
	fDirty = true;
	return fImage->Write(fDataOffset + fSize + (u64)sector * kTagSize, tag,
		kTagSize);
}


// Disk Copy's checksums: each big endian word added, then the sum turned
// right by a bit. The tags' leave out the first sector's.
bool
FloppyImage::_UpdateChecksums()
{
	auto checksum = [this](u64 offset, u64 length, u32& sum) {
		std::vector<u8> buffer(64 * 1024);
		sum = 0;
		while (length > 0) {
			size_t chunk = (size_t)std::min<u64>(buffer.size(), length);
			if (!fImage->Read(offset, buffer.data(), chunk))
				return false;
			for (size_t i = 0; i + 1 < chunk; i += 2) {
				sum += LoadBE16(&buffer[i]);
				sum = (sum >> 1) | (sum << 31);
			}
			offset += chunk;
			length -= chunk;
		}
		return true;
	};
	u32 dataSum;
	u32 tagSum = 0;
	if (!checksum(fDataOffset, fSize, dataSum)
		|| (fTagSize > kTagSize && !checksum(fDataOffset + fSize + kTagSize,
			fTagSize - kTagSize, tagSum)))
		return false;
	u8 sums[8];
	StoreBE32(sums, dataSum);
	StoreBE32(sums + 4, tagSum);
	return fImage->Write(72, sums, sizeof(sums));
}


bool
FloppyImage::Flush()
{
	bool ok = true;
	if (fDirty && IsDiskCopy() && !fImage->IsReadOnly())
		ok = _UpdateChecksums();
	fDirty = false;
	return fImage->Flush() && ok;
}

}	// namespace powermac
