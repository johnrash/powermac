/*
 * PowerMac emulator - floppy disk images.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Wraps a disk image file as a floppy disk of one of the formats the
 * SuperDrive takes: 400 KB (one side) and 800 KB GCR disks (Mac OS's own),
 * 720 KB and 1.44 MB MFM disks. The file is either the sectors one after
 * the other ("raw", as SheepShaver's and dd's), possibly cut short where
 * only zeros followed (a short file is padded with zeros; the format then
 * comes from the HFS, MFS or FAT volume on it), or a Disk Copy 4.2 image:
 * an 84 byte header (name, data and tag sizes and checksums, the format),
 * the sectors, then 12 tag bytes a sector. The checksums of a Disk Copy
 * image are brought up to date when it was written to.
 *
 * A compressed Disk Copy 6 image (NDIF) that lost its resource fork, the
 * map of its chunks, is put back together when that can be done one way
 * only: an HFS volume whose used blocks are one run from the first (as on
 * the disks Disk Copy made), stored as the sectors before the blocks, the
 * used blocks in chunks of 512 sectors (each ADC compressed or stored) and
 * the spare master directory block. Such a disk is read only, as Disk Copy
 * mounts compressed images. Others are refused.
 */
#ifndef PM_FLOPPY_IMAGE_H
#define PM_FLOPPY_IMAGE_H

#include "DiskImage.h"

#include <memory>
#include <string>

namespace powermac {

class FloppyImage : public DiskImage {
public:
	enum Kind {
		kGcr400,		// 80 tracks, one side, 8-12 sectors a track
		kGcr800,		// the same on two sides
		kMfm720,		// 80 tracks, two sides, 9 sectors
		kMfm1440		// 18 sectors
	};

	static const u32 kTagSize = 12;

	// The floppy in 'image'; null (with 'error') if it is none.
	static	std::unique_ptr<FloppyImage> Wrap(
									std::unique_ptr<DiskImage> image,
									std::string& error);

	virtual						~FloppyImage();

			Kind				GetKind() const { return fKind; }
			bool				IsGcr() const
									{ return fKind == kGcr400
										|| fKind == kGcr800; }
			int					Sides() const
									{ return fKind == kGcr400 ? 1 : 2; }
			bool				IsDiskCopy() const { return fDataOffset != 0; }

			// The tag bytes of a 512 byte sector (zeros without tags).
			bool				ReadTag(u32 sector, u8 tag[kTagSize]);
			bool				WriteTag(u32 sector, const u8 tag[kTagSize]);

	// DiskImage: the disk's sectors
	virtual	u64					Size() const { return fSize; }
	virtual	bool				IsReadOnly() const
									{ return fImage->IsReadOnly(); }
	virtual	bool				Read(u64 offset, void* buffer, size_t length);
	virtual	bool				Write(u64 offset, const void* buffer,
									size_t length);
	virtual	bool				Flush();
	virtual	std::string			Path() const { return fImage->Path(); }
	virtual	const char*			Format() const
									{ return IsDiskCopy() ? "Disk Copy 4.2"
										: "raw"; }

	// The size of a disk of a kind.
	static	u64					KindSize(Kind kind);

private:
								FloppyImage(std::unique_ptr<DiskImage> image,
									Kind kind, u64 dataOffset, u64 tagSize);

			bool				_UpdateChecksums();

			std::unique_ptr<DiskImage> fImage;
			Kind				fKind;
			u64					fSize;
			u64					fDataOffset;	// Disk Copy: 84
			u64					fTagSize;		// Disk Copy tags
			bool				fDirty;
};

}	// namespace powermac

#endif	// PM_FLOPPY_IMAGE_H
