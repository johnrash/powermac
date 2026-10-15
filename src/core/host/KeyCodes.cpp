/*
 * PowerMac emulator - keyboard codes.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "KeyCodes.h"

#include <cstring>
#include <strings.h>

namespace powermac {

namespace {

const u8 kNoHaikuKey = 0;

struct Key {
	u8 haiku;		// kNoHaikuKey: a name only
	u8 adb;
	const char* name;
};

const Key kKeys[] = {
	{ 0x01, 0x35, "Escape" },
	{ 0x02, 0x7a, "F1" }, { 0x03, 0x78, "F2" }, { 0x04, 0x63, "F3" },
	{ 0x05, 0x76, "F4" }, { 0x06, 0x60, "F5" }, { 0x07, 0x61, "F6" },
	{ 0x08, 0x62, "F7" }, { 0x09, 0x64, "F8" }, { 0x0a, 0x65, "F9" },
	{ 0x0b, 0x6d, "F10" }, { 0x0c, 0x67, "F11" }, { 0x0d, 0x6f, "F12" },
	{ 0x0e, 0x69, "F13" }, { 0x0f, 0x6b, "F14" }, { 0x10, 0x71, "F15" },
	{ 0x11, 0x32, "Grave" },
	{ 0x12, 0x12, "1" }, { 0x13, 0x13, "2" }, { 0x14, 0x14, "3" },
	{ 0x15, 0x15, "4" }, { 0x16, 0x17, "5" }, { 0x17, 0x16, "6" },
	{ 0x18, 0x1a, "7" }, { 0x19, 0x1c, "8" }, { 0x1a, 0x19, "9" },
	{ 0x1b, 0x1d, "0" }, { 0x1c, 0x1b, "Minus" }, { 0x1d, 0x18, "Equal" },
	{ 0x1e, 0x33, "Delete" },
	{ 0x1f, 0x72, "Help" }, { 0x20, 0x73, "Home" },
	{ 0x21, 0x74, "PageUp" },
	{ 0x22, 0x47, "Clear" }, { 0x23, 0x4b, "KeypadSlash" },
	{ 0x24, 0x43, "KeypadStar" }, { 0x25, 0x4e, "KeypadMinus" },
	{ 0x26, 0x30, "Tab" },
	{ 0x27, 0x0c, "Q" }, { 0x28, 0x0d, "W" }, { 0x29, 0x0e, "E" },
	{ 0x2a, 0x0f, "R" }, { 0x2b, 0x11, "T" }, { 0x2c, 0x10, "Y" },
	{ 0x2d, 0x20, "U" }, { 0x2e, 0x22, "I" }, { 0x2f, 0x1f, "O" },
	{ 0x30, 0x23, "P" }, { 0x31, 0x21, "LeftBracket" },
	{ 0x32, 0x1e, "RightBracket" }, { 0x33, 0x2a, "Backslash" },
	{ 0x34, 0x75, "ForwardDelete" }, { 0x35, 0x77, "End" },
	{ 0x36, 0x79, "PageDown" },
	{ 0x37, 0x59, "Keypad7" }, { 0x38, 0x5b, "Keypad8" },
	{ 0x39, 0x5c, "Keypad9" }, { 0x3a, 0x45, "KeypadPlus" },
	{ 0x3b, 0x39, "CapsLock" },
	{ 0x3c, 0x00, "A" }, { 0x3d, 0x01, "S" }, { 0x3e, 0x02, "D" },
	{ 0x3f, 0x03, "F" }, { 0x40, 0x05, "G" }, { 0x41, 0x04, "H" },
	{ 0x42, 0x26, "J" }, { 0x43, 0x28, "K" }, { 0x44, 0x25, "L" },
	{ 0x45, 0x29, "Semicolon" }, { 0x46, 0x27, "Quote" },
	{ 0x47, 0x24, "Return" },
	{ 0x48, 0x56, "Keypad4" }, { 0x49, 0x57, "Keypad5" },
	{ 0x4a, 0x58, "Keypad6" },
	{ 0x4b, 0x38, "Shift" },
	{ 0x4c, 0x06, "Z" }, { 0x4d, 0x07, "X" }, { 0x4e, 0x08, "C" },
	{ 0x4f, 0x09, "V" }, { 0x50, 0x0b, "B" }, { 0x51, 0x2d, "N" },
	{ 0x52, 0x2e, "M" }, { 0x53, 0x2b, "Comma" }, { 0x54, 0x2f, "Period" },
	{ 0x55, 0x2c, "Slash" }, { 0x56, 0x7b, "RightShift" },
	{ 0x57, 0x3e, "Up" },
	{ 0x58, 0x53, "Keypad1" }, { 0x59, 0x54, "Keypad2" },
	{ 0x5a, 0x55, "Keypad3" }, { 0x5b, 0x4c, "KeypadEnter" },
	{ 0x5c, 0x36, "Control" }, { 0x5d, 0x37, "Command" },
	{ 0x5e, 0x31, "Space" }, { 0x5f, 0x37, "RightCommand" },
	{ 0x60, 0x7d, "RightControl" },
	{ 0x61, 0x3b, "Left" }, { 0x62, 0x3d, "Down" },
	{ 0x63, 0x3c, "Right" },
	{ 0x64, 0x52, "Keypad0" }, { 0x65, 0x41, "KeypadPeriod" },
	{ 0x66, 0x3a, "Option" }, { 0x67, 0x7c, "RightOption" },
	{ 0x69, 0x0a, "Section" },
	{ kNoHaikuKey, 0x51, "KeypadEqual" },
	{ kNoHaikuKey, 0x7f, "Power" },
	// the names of the AT keys these take the place of
	{ kNoHaikuKey, 0x24, "Enter" },
	{ kNoHaikuKey, 0x33, "Backspace" },
	{ kNoHaikuKey, 0x38, "LeftShift" },
	{ kNoHaikuKey, 0x36, "LeftControl" },
	{ kNoHaikuKey, 0x37, "LeftAlt" },
	{ kNoHaikuKey, 0x3a, "LeftWindows" },
	{ kNoHaikuKey, 0x72, "Insert" },
};

}	// namespace


int
HaikuKeyToAdb(u32 haikuKey)
{
	if (haikuKey == kNoHaikuKey)
		return -1;
	for (const Key& key : kKeys) {
		if (key.haiku == haikuKey)
			return key.adb;
	}
	return -1;
}


int
KeyNameToAdb(const char* name)
{
	for (const Key& key : kKeys) {
		if (strcasecmp(key.name, name) == 0)
			return key.adb;
	}
	return -1;
}


int
AsciiToAdb(char c, bool& shifted)
{
	static const char* const kShifted = "~!@#$%^&*()_+{}|:\"<>?";
	static const char* const kUnshifted = "`1234567890-=[]\\;',./";
	static const char* const kNames[] = { "Grave", "1", "2", "3", "4", "5",
		"6", "7", "8", "9", "0", "Minus", "Equal", "LeftBracket",
		"RightBracket", "Backslash", "Semicolon", "Quote", "Comma", "Period",
		"Slash" };
	shifted = false;
	char name[2] = { 0, 0 };
	if (c >= 'a' && c <= 'z') {
		name[0] = (char)(c - 32);
		return KeyNameToAdb(name);
	}
	if (c >= 'A' && c <= 'Z') {
		name[0] = c;
		shifted = true;
		return KeyNameToAdb(name);
	}
	switch (c) {
		case ' ': return KeyNameToAdb("Space");
		case '\n': case '\r': return KeyNameToAdb("Return");
		case '\t': return KeyNameToAdb("Tab");
		case '\b': return KeyNameToAdb("Delete");
		case 0: return -1;
	}
	if (const char* p = strchr(kShifted, c)) {
		shifted = true;
		return KeyNameToAdb(kNames[p - kShifted]);
	}
	if (const char* p = strchr(kUnshifted, c))
		return KeyNameToAdb(kNames[p - kUnshifted]);
	return -1;
}

}	// namespace powermac
