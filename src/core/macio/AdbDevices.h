/*
 * PowerMac emulator - the Apple Desktop Bus and its devices.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * An ADB command byte is (address << 4) | (command << 2) | register:
 * command 0 with register 0 is SendReset, 0 with register 1 Flush, 2
 * Listen (data follows) and 3 Talk (the device answers with up to 8 bytes
 * or not at all: a timeout).
 *
 * Register 3 of every device: bit 14 exceptional event, bit 13 service
 * request enable, bits 11-8 the address, bits 7-0 the device handler ID.
 * A Listen to register 3 moves the device to another address (handler
 * 0xFE: unless another device answered the last Talk too, which never
 * happens here: each device has its address to itself) or selects
 * another handler the device supports.
 *
 * Devices:
 * - the keyboard (address 2): an Apple Extended Keyboard (handler 2;
 *   handler 3 reports the right modifier keys apart); register 0 holds up
 *   to two key transitions (bit 7: released), register 2 the modifiers
 *   and the LEDs (active low);
 * - the mouse (address 3, handler 1 at 100 or 2 at 200 counts per inch):
 *   register 0 is the button (bit 15, 0 = down) and the motion since the
 *   last report (Y in bits 14-8, X in bits 6-0, 7-bit two's complement,
 *   +Y down).
 */
#ifndef PM_ADB_DEVICES_H
#define PM_ADB_DEVICES_H

#include "../Types.h"

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace powermac {

class AdbDevice {
public:
								AdbDevice(u8 defaultAddress, u8 handler);
	virtual						~AdbDevice() {}

			u8					Address() const { return fAddress; }
			u8					Handler() const { return fHandler; }

	virtual	void				Reset();
			// Talk: false when the device does not answer (nothing to
			// report in register 0).
	virtual	bool				Talk(int reg, std::vector<u8>& data);
	virtual	void				Listen(int reg, const u8* data, int length);
	virtual	void				Flush() {}
			// Has something to report (it would ask for service).
	virtual	bool				HasData() const { return false; }
	virtual	const char*			Name() const = 0;

protected:
	virtual	bool				SupportsHandler(u8 handler) const
									{ return handler == fDefaultHandler; }

			u8					fDefaultAddress;
			u8					fDefaultHandler;
			u8					fAddress;
			u8					fHandler;
			bool				fServiceRequests;
};


class AdbKeyboard : public AdbDevice {
public:
								AdbKeyboard();

	virtual	void				Reset();
	virtual	bool				Talk(int reg, std::vector<u8>& data);
	virtual	void				Listen(int reg, const u8* data, int length);
	virtual	void				Flush();
	virtual	bool				HasData() const;
	virtual	const char*			Name() const { return "keyboard"; }

			// From the host side (any thread).
			void				KeyDown(u8 code);
			void				KeyUp(u8 code);

protected:
	virtual	bool				SupportsHandler(u8 handler) const
									{ return handler == 2 || handler == 3; }

private:
			void				_UpdateModifiers(u8 code, bool down);
			void				_ReportHeldKeys();

	mutable	std::mutex			fLock;
			std::deque<u8>		fEvents;		// code | 0x80 = released
			u16					fRegister2;		// modifiers, LEDs
			// The keys down: after a reset or a flush the keyboard reports
			// them again, as its scan finds them (keys held at power on).
			bool				fDown[128];
};


class AdbMouse : public AdbDevice {
public:
								AdbMouse();

	virtual	void				Reset();
	virtual	bool				Talk(int reg, std::vector<u8>& data);
	virtual	void				Flush();
	virtual	bool				HasData() const;
	virtual	const char*			Name() const { return "mouse"; }

			// From the host side (any thread): motion in counts (+y down)
			// and the button.
			void				Moved(int dx, int dy, bool button);

protected:
	virtual	bool				SupportsHandler(u8 handler) const
									{ return handler == 1 || handler == 2; }

private:
	mutable	std::mutex			fLock;
			int					fDx;
			int					fDy;
			bool				fButton;
			bool				fReportedButton;
};


class AdbBus {
public:
								AdbBus();

			void				Reset();
			AdbKeyboard&		Keyboard() { return *fKeyboard; }
			AdbMouse&			Mouse() { return *fMouse; }

			// Runs a command: false on a timeout (no device answered a
			// Talk).
			bool				Command(u8 command, const u8* data,
									int length, std::vector<u8>& reply);
			// The first device with something to report (in 'mask', a
			// bitmap of addresses; 0: any), or null.
			AdbDevice*			DeviceWithData(u16 mask);

private:
			AdbDevice*			_DeviceAt(int address);

			std::unique_ptr<AdbKeyboard> fKeyboard;
			std::unique_ptr<AdbMouse> fMouse;
			AdbDevice*			fDevices[2];
};

}	// namespace powermac

#endif	// PM_ADB_DEVICES_H
