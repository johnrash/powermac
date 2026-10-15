/*
 * PowerMac emulator - what the shared folders (BFS and HFS) share: names,
 * snapshot text and times.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_SHARED_FOLDER_UTIL_H
#define PM_SHARED_FOLDER_UTIL_H

#include "../Types.h"

#include <sys/stat.h>

#include <string>

namespace powermac {

inline s64
ModifiedNs(const struct stat& st)
{
	return (s64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
}


inline std::string
Escape(const std::string& text)
{
	std::string result;
	for (char c : text) {
		if (c == '\\')
			result += "\\\\";
		else if (c == '\t')
			result += "\\t";
		else if (c == '\n')
			result += "\\n";
		else
			result += c;
	}
	return result;
}


inline std::string
Unescape(const std::string& text)
{
	std::string result;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			char c = text[++i];
			result += c == 't' ? '\t' : c == 'n' ? '\n' : c;
		} else
			result += text[i];
	}
	return result;
}


inline std::string
Leaf(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


// "NAME (PowerMac)", numbered when that exists too.
inline std::string
ConflictName(const std::string& path)
{
	std::string base = path + " (PowerMac)";
	struct stat st;
	if (lstat(base.c_str(), &st) != 0)
		return base;
	for (int i = 2; i < 1000; i++) {
		std::string name = path + " (PowerMac " + std::to_string(i) + ")";
		if (lstat(name.c_str(), &st) != 0)
			return name;
	}
	return base;
}

}	// namespace powermac

#endif	// PM_SHARED_FOLDER_UTIL_H
