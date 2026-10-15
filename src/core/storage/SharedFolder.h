/*
 * PowerMac emulator - a host folder shared with BeOS as a SCSI disk.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * At power on the folder's files, directories and attributes are written
 * to a fresh BFS volume (a thin QCOW2 image); a snapshot records what was
 * written. After BeOS shut down cleanly, the volume is read back and its
 * changes go to the folder: new and modified files, attributes, deletions.
 * A file changed on both sides keeps the host version and gets BeOS's as
 * "NAME (PowerMac)". A volume BeOS did not unmount cleanly is kept and used
 * again at the next power on, so that BeOS replays its log first.
 *
 * BFS on the BeBox is big endian: attributes of known numeric types are
 * byte swapped on the way (the host's are in its own byte order).
 */
#ifndef PM_SHARED_FOLDER_H
#define PM_SHARED_FOLDER_H

#include "BfsWriter.h"

#include <string>
#include <vector>

namespace powermac {

class DiskImage;

// The host's file attributes (Haiku's; none elsewhere).
class HostAttributes {
public:
	virtual						~HostAttributes() {}
	// Attribute data in the host's byte order.
	virtual	bool				Read(const std::string& path,
									std::vector<BfsAttribute>& attributes)
									= 0;
	virtual	bool				Write(const std::string& path,
									const std::vector<BfsAttribute>&
										attributes) = 0;
	// One attribute's data and size (false if the file has none).
	virtual	bool				ReadAttribute(const std::string& path,
									const std::string& name,
									std::vector<u8>& data);
	virtual	bool				AttributeSize(const std::string& path,
									const std::string& name, u64& size);
};

// A host folder shared as a disk: the volume is built at power on and its
// changes go back to the folder after the machine stopped.
class SharedVolume {
public:
	virtual						~SharedVolume() {}

			// Power on: syncs a volume an earlier session left behind, then
			// builds the image from the folder. 'report' tells what
			// happened besides (empty if nothing).
	virtual	bool				Prepare(std::string& report,
									std::string& error) = 0;
			// After the machine stopped: the system's changes to the folder.
	virtual	bool				Sync(std::string& report,
									std::string& error) = 0;
};

class SharedFolder : public SharedVolume {
public:
								SharedFolder(const std::string& folder,
									const std::string& image,
									HostAttributes* attributes = nullptr);

			const std::string&	Folder() const { return fFolder; }
			const std::string&	ImagePath() const { return fImage; }

	virtual	bool				Prepare(std::string& report,
									std::string& error);
	virtual	bool				Sync(std::string& report, std::string& error);

	// Byte swaps attribute data of known numeric types (BFS on the BeBox is
	// big endian); symmetric.
	static	void				SwapAttribute(u32 type,
									std::vector<u8>& data);

private:
	struct Record {
		bool directory = false;
		u64 hostSize = 0;
		s64 hostModified = 0;		// ns
		s64 bfsModified = 0;		// as written
		u64 bfsSize = 0;
		u64 attributesHash = 0;
	};

			bool				_Build(std::string& error);
			bool				_Sync(DiskImage& image, std::string& report,
									std::string& error);
			bool				_ScanFolder(const std::string& relative,
									BfsWriter::Node& node, u64& bytes,
									std::string& error);
			void				_Record(const std::string& relative,
									const BfsWriter::Node& node);
			bool				_SaveSnapshot();
			bool				_LoadSnapshot();
			std::vector<BfsAttribute> _HostAttributes(
									const std::string& path);
			void				_WriteHostAttributes(const std::string& path,
									std::vector<BfsAttribute> attributes);

			std::string			fFolder;
			std::string			fSnapshotFolder;
			std::string			fImage;
			HostAttributes*		fAttributes;
			std::vector<std::pair<std::string, Record> > fRecords;
};

}	// namespace powermac

#endif	// PM_SHARED_FOLDER_H
