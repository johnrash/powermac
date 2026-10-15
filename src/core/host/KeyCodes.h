/*
 * PowerMac emulator - keyboard codes.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Maps Haiku/BeOS key codes (layout independent key positions) to the raw
 * key codes of an Apple Extended Keyboard on ADB (A is 0, Control 0x36,
 * the arrows 0x3B-0x3E, the right modifiers 0x7B-0x7D, Power 0x7F).
 * Haiku's Alt keys are Command and its Windows keys Option, as Haiku itself
 * places its modifiers.
 */
#ifndef PM_KEY_CODES_H
#define PM_KEY_CODES_H

#include "../Types.h"

namespace powermac {

// The ADB code; -1 for keys without an equivalent.
int HaikuKeyToAdb(u32 haikuKey);

// Key names for scripts ("Return", "A", "F1", "Up", "Command", ...); -1 if
// unknown.
int KeyNameToAdb(const char* name);

// The key typing an ASCII character on a US keyboard ('\n' is Return, '\t'
// Tab); 'shifted' tells whether Shift is needed. -1 if there is none.
int AsciiToAdb(char c, bool& shifted);

}	// namespace powermac

#endif	// PM_KEY_CODES_H
