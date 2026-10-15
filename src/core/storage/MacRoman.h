/*
 * PowerMac emulator - Mac OS Roman text and HFS names.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * HFS names are Mac OS Roman bytes (1-31 of them; volume names 1-27),
 * without colons, ordered and matched as Mac OS's File Manager does: the
 * ROM's RelString without case (each byte uppercased, then put in the
 * order of its base letter and accent: "a" and "A" are the same name,
 * "a" and "á" are not).
 */
#ifndef PM_MAC_ROMAN_H
#define PM_MAC_ROMAN_H

#include "../Types.h"

#include <string>

namespace powermac {

// Host names (UTF-8) and HFS names: ':' (the HFS path separator) and '/'
// (the host's) trade places; characters Mac OS Roman lacks become '_'.
std::string		HostToMacName(const std::string& utf8);
std::string		MacToHostName(const std::string& macRoman);

// The order of names in an HFS catalog (< 0, 0, > 0).
int				CompareHfsNames(const std::string& a, const std::string& b);

}	// namespace powermac

#endif	// PM_MAC_ROMAN_H
