/*
 * PowerMac emulator - the Apple Desktop Bus and its devices.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "AdbDevices.h"

#include "../Log.h"

#include <algorithm>
#include <cstring>

namespace powermac {

// ADB key codes of the modifier keys and their register 2 bits.
enum {
	kKeyDelete = 0x33,
	kKeyCommand = 0x37,
	kKeyShift = 0x38,
	kKeyCapsLock = 0x39,
	kKeyOption = 0x3a,
	kKeyControl = 0x36,
	kKeyNumLock = 0x47,
	kKeyScrollLock = 0x6b,
	kKeyRightShift = 0x7b,
	kKeyRightOption = 0x7c,
	kKeyRightControl = 0x7d,
	kKeyPower = 0x7f
};


AdbDevice::AdbDevice(u8 defaultAddress, u8 handler)
	:
	fDefaultAddress(defaultAddress),
	fDefaultHandler(handler)
{
	AdbDevice::Reset();
}


void
AdbDevice::Reset()
{
	fAddress = fDefaultAddress;
	fHandler = fDefaultHandler;
	fServiceRequests = true;
}


bool
AdbDevice::Talk(int reg, std::vector<u8>& data)
{
	if (reg != 3)
		return false;
	// a random value in the address field's place in the real thing
	data.push_back((fServiceRequests ? 0x20 : 0) | 0x40 | fAddress);
	data.push_back(fHandler);
	return true;
}


void
AdbDevice::Listen(int reg, const u8* data, int length)
{
	if (reg != 3 || length < 2)
		return;
	u8 handler = data[1];
	switch (handler) {
		case 0x00:
			// change the address and the service request enable
			fAddress = data[0] & 0x0f;
			fServiceRequests = (data[0] & 0x20) != 0;
			break;
		case 0xfd:		// change the address if the activator is pressed
		case 0xfe:		// change the address unless there was a collision
			fAddress = data[0] & 0x0f;
			break;
		case 0xff:		// self test
			break;
		default:
			if (SupportsHandler(handler))
				fHandler = handler;
			break;
	}
	PM_LOG(kLogCuda, kLogDebug, "adb %s: address %d, handler %d", Name(),
		fAddress, fHandler);
}


// #pragma mark - keyboard


AdbKeyboard::AdbKeyboard()
	:
	AdbDevice(2, 2)
{
	memset(fDown, 0, sizeof(fDown));
	AdbKeyboard::Reset();
}


void
AdbKeyboard::Reset()
{
	AdbDevice::Reset();
	std::lock_guard<std::mutex> lock(fLock);
	fRegister2 = 0xffff;
	for (int code = 0; code < 128; code++) {
		if (fDown[code])
			_UpdateModifiers(code, true);
	}
	_ReportHeldKeys();
}


void
AdbKeyboard::Flush()
{
	std::lock_guard<std::mutex> lock(fLock);
	_ReportHeldKeys();
}


void
AdbKeyboard::_ReportHeldKeys()
{
	fEvents.clear();
	for (int code = 0; code < 128; code++) {
		if (fDown[code] && code != kKeyPower)
			fEvents.push_back(code);
	}
}


bool
AdbKeyboard::HasData() const
{
	std::lock_guard<std::mutex> lock(fLock);
	return !fEvents.empty();
}


void
AdbKeyboard::_UpdateModifiers(u8 code, bool down)
{
	int bit;
	switch (code) {
		case kKeyDelete: bit = 14; break;
		case kKeyCapsLock: bit = 13; break;
		case kKeyPower: bit = 12; break;
		case kKeyControl: case kKeyRightControl: bit = 11; break;
		case kKeyShift: case kKeyRightShift: bit = 10; break;
		case kKeyOption: case kKeyRightOption: bit = 9; break;
		case kKeyCommand: bit = 8; break;
		case kKeyNumLock: bit = 7; break;
		case kKeyScrollLock: bit = 6; break;
		default: return;
	}
	if (down)
		fRegister2 &= ~(1u << bit);
	else
		fRegister2 |= 1u << bit;
}


void
AdbKeyboard::KeyDown(u8 code)
{
	std::lock_guard<std::mutex> lock(fLock);
	_UpdateModifiers(code, true);
	fDown[code & 0x7f] = true;
	fEvents.push_back(code & 0x7f);
	if (code == kKeyPower)
		fEvents.push_back(kKeyPower);	// the power key comes twice
}


void
AdbKeyboard::KeyUp(u8 code)
{
	std::lock_guard<std::mutex> lock(fLock);
	_UpdateModifiers(code, false);
	fDown[code & 0x7f] = false;
	fEvents.push_back((code & 0x7f) | 0x80);
	if (code == kKeyPower)
		fEvents.push_back(kKeyPower | 0x80);
}


bool
AdbKeyboard::Talk(int reg, std::vector<u8>& data)
{
	std::lock_guard<std::mutex> lock(fLock);
	switch (reg) {
		case 0:
		{
			if (fEvents.empty())
				return false;
			for (int i = 0; i < 2; i++) {
				u8 event = 0xff;
				if (!fEvents.empty()) {
					event = fEvents.front();
					fEvents.pop_front();
					// handler 2 reports the right modifiers as the left
					// ones
					u8 code = event & 0x7f;
					if (fHandler == 2 && code >= kKeyRightShift
						&& code <= kKeyRightControl) {
						static const u8 kLeft[] = { kKeyShift, kKeyOption,
							kKeyControl };
						event = (event & 0x80) | kLeft[code - kKeyRightShift];
					}
				}
				data.push_back(event);
			}
			return true;
		}
		case 2:
			data.push_back((u8)(fRegister2 >> 8));
			data.push_back((u8)fRegister2);
			return true;
		case 3:
			return AdbDevice::Talk(reg, data);
	}
	return false;
}


void
AdbKeyboard::Listen(int reg, const u8* data, int length)
{
	if (reg == 2 && length >= 2) {
		// the LEDs (bits 0-2)
		std::lock_guard<std::mutex> lock(fLock);
		fRegister2 = (fRegister2 & ~7u) | (data[1] & 7);
		return;
	}
	AdbDevice::Listen(reg, data, length);
}


// #pragma mark - mouse


AdbMouse::AdbMouse()
	:
	AdbDevice(3, 2)
{
	AdbMouse::Reset();
}


void
AdbMouse::Reset()
{
	AdbDevice::Reset();
	std::lock_guard<std::mutex> lock(fLock);
	fDx = fDy = 0;
	fButton = fReportedButton = false;
}


void
AdbMouse::Flush()
{
	std::lock_guard<std::mutex> lock(fLock);
	fDx = fDy = 0;
	fReportedButton = fButton;
}


bool
AdbMouse::HasData() const
{
	std::lock_guard<std::mutex> lock(fLock);
	return fDx != 0 || fDy != 0 || fButton != fReportedButton;
}


void
AdbMouse::Moved(int dx, int dy, bool button)
{
	std::lock_guard<std::mutex> lock(fLock);
	fDx += dx;
	fDy += dy;
	fButton = button;
}


bool
AdbMouse::Talk(int reg, std::vector<u8>& data)
{
	if (reg == 3)
		return AdbDevice::Talk(reg, data);
	if (reg != 0)
		return false;
	std::lock_guard<std::mutex> lock(fLock);
	if (fDx == 0 && fDy == 0 && fButton == fReportedButton)
		return false;
	int dx = std::max(-64, std::min(63, fDx));
	int dy = std::max(-64, std::min(63, fDy));
	fDx -= dx;
	fDy -= dy;
	fReportedButton = fButton;
	data.push_back((fButton ? 0x00 : 0x80) | (dy & 0x7f));
	data.push_back(0x80 | (dx & 0x7f));
	return true;
}


// #pragma mark - bus


AdbBus::AdbBus()
	:
	fKeyboard(new AdbKeyboard),
	fMouse(new AdbMouse)
{
	fDevices[0] = fKeyboard.get();
	fDevices[1] = fMouse.get();
}


void
AdbBus::Reset()
{
	for (AdbDevice* device : fDevices)
		device->Reset();
}


AdbDevice*
AdbBus::_DeviceAt(int address)
{
	for (AdbDevice* device : fDevices) {
		if (device->Address() == address)
			return device;
	}
	return nullptr;
}


bool
AdbBus::Command(u8 command, const u8* data, int length,
	std::vector<u8>& reply)
{
	int address = command >> 4;
	int reg = command & 3;
	switch ((command >> 2) & 3) {
		case 0:
			if ((command & 0x0f) == 0) {
				// SendReset: all devices
				Reset();
				return true;
			}
			if ((command & 0x0f) == 1) {
				AdbDevice* device = _DeviceAt(address);
				if (device != nullptr)
					device->Flush();
				return device != nullptr;
			}
			return true;
		case 2:
		{
			AdbDevice* device = _DeviceAt(address);
			if (device == nullptr)
				return false;
			device->Listen(reg, data, length);
			return true;
		}
		case 3:
		{
			AdbDevice* device = _DeviceAt(address);
			return device != nullptr && device->Talk(reg, reply);
		}
	}
	return false;
}


AdbDevice*
AdbBus::DeviceWithData(u16 mask)
{
	for (AdbDevice* device : fDevices) {
		if ((mask == 0 || (mask & (1u << device->Address())) != 0)
			&& device->HasData())
			return device;
	}
	return nullptr;
}

}	// namespace powermac
