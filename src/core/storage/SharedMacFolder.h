/*
 * PowerMac emulator - a host folder shared with Mac OS as a SCSI disk.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * At power on the folder's files and folders are written to a fresh HFS
 * volume (HfsWriter) on a disk with an Apple partition map and a copy of
 * the SCSI driver of a disk Drive Setup initialized (ApplePartitionMap):
 * without a driver on the disk Mac OS does not mount it. A snapshot
 * records what was written. After Mac OS shut down (which leaves the
 * volume marked unmounted), the volume is read back (HfsReader) and its
 * changes go to the folder: new and modified files, Finder information,
 * resource forks, deletions. A file changed on both sides keeps the host
 * version and gets Mac OS's as "NAME (PowerMac)". A volume Mac OS did not
 * unmount is kept and used again at the next power on.
 *
 * Names: HFS has 31 bytes of Mac OS Roman and ignores case; a longer or
 * clashing name gets "#N" before its extension, and the snapshot maps it
 * back to the host name.
 *
 * Mac metadata lives in attributes where the host has them (Haiku), named
 * as SheepShaver's do on BeOS: MACOS:RFORK (the resource fork),
 * MACOS:CREATOR and MACOS:HFS_FLAGS (big endian, as on the Mac), and
 * MACOS:FINDER_INFO (the 32 bytes of FInfo and FXInfo, or DInfo and DXInfo:
 * type, icon position, window). A file without them gets a type and
 * creator from its MIME type (BEOS:TYPE) or extension; a file new from Mac
 * OS gets a MIME type from its type.
 */
#ifndef PM_SHARED_MAC_FOLDER_H
#define PM_SHARED_MAC_FOLDER_H

#include "HfsReader.h"
#include "HfsWriter.h"
#include "SharedFolder.h"

#include <map>
#include <string>
#include <vector>

namespace powermac {

class DiskImage;

class SharedMacFolder : public SharedVolume {
public:
	// 'driverDisks': disk images to take the SCSI driver from, the first
	// one Drive Setup initialized.
								SharedMacFolder(const std::string& folder,
									const std::string& image,
									const std::vector<std::string>&
										driverDisks,
									HostAttributes* attributes = nullptr);

			const std::string&	Folder() const { return fFolder; }
			const std::string&	ImagePath() const { return fImage; }

	virtual	bool				Prepare(std::string& report,
									std::string& error);
	virtual	bool				Sync(std::string& report, std::string& error);

	// The Mac OS type and creator for a host file without them (0 if none
	// fits), from its MIME type or else its name's extension.
	static	void				TypeAndCreator(const std::string& name,
									const std::string& mimeType, u32& type,
									u32& creator);
	// The MIME type for a Mac OS type ("" if none).
	static	const char*			MimeType(u32 type);

	// Mac OS keeps the time of day; the host's times are UTC.
	static	u32					ToMacTime(s64 seconds);
	static	s64					FromMacTime(u32 macTime);

private:
	struct Record {
		std::string			hostPath;		// relative ("" for the root)
		bool				directory = false;
		u64					hostSize = 0;
		s64					hostModified = 0;	// ns
		u32					macModified = 0;	// as written
		u64					dataSize = 0;
		u64					resourceSize = 0;
		u64					infoHash = 0;
	};

			bool				_Build(std::string& error);
			bool				_Scan(const std::string& relative,
									const std::string& hfsPath,
									HfsWriter::Node& node, u64& bytes,
									std::string& error);
			void				_FinderInfo(const std::string& path,
									const std::string& name, bool directory,
									bool atRoot, u8 info[32]);
			bool				_Sync(DiskImage& image, std::string& report,
									std::string& error);
			void				_WriteMacAttributes(const std::string& path,
									const HfsReader::Entry& entry,
									const HfsReader& reader,
									bool resourceToo, bool newFile);
			bool				_SaveSnapshot();
			bool				_LoadSnapshot();

			std::string			fFolder;
			std::string			fSnapshotFolder;
			std::string			fImage;
			std::vector<std::string> fDriverDisks;
			HostAttributes*		fAttributes;
			std::map<std::string, Record> fRecords;	// by HFS path
};

}	// namespace powermac

#endif	// PM_SHARED_MAC_FOLDER_H
