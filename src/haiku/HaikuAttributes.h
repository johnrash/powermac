/*
 * PowerMac emulator for Haiku - file attributes of shared folders.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Gives the shared folder code (src/core/storage/SharedFolder and
 * SharedMacFolder) access to Haiku's file attributes, so that file types,
 * icons and Tracker's state travel between Haiku and BeOS, and resource
 * forks and Finder information between Haiku and Mac OS.
 */
#ifndef PM_HAIKU_ATTRIBUTES_H
#define PM_HAIKU_ATTRIBUTES_H

#include "core/storage/SharedFolder.h"

class HaikuAttributes : public powermac::HostAttributes {
public:
	virtual	bool				Read(const std::string& path,
									std::vector<powermac::BfsAttribute>&
										attributes);
	virtual	bool				Write(const std::string& path,
									const std::vector<powermac::BfsAttribute>&
										attributes);
	virtual	bool				ReadAttribute(const std::string& path,
									const std::string& name,
									std::vector<powermac::u8>& data);
	virtual	bool				AttributeSize(const std::string& path,
									const std::string& name,
									powermac::u64& size);
};

#endif	// PM_HAIKU_ATTRIBUTES_H
