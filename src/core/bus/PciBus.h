/*
 * PowerMac emulator - a PCI bus and configuration space.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * One bus behind each Bandit (devices 11-31); function 0 only. Devices
 * declare their base address registers and, for cards, an expansion ROM
 * (configuration register 0x30): its address, size mask and enable bit,
 * decoded only while memory space is enabled.
 */
#ifndef PM_PCI_BUS_H
#define PM_PCI_BUS_H

#include "../Types.h"

namespace powermac {

class PciBus;

class PciDevice {
public:
	enum BarType {
		kBarNone,
		kBarMemory,
		kBarIo
	};

								PciDevice(u16 vendor, u16 device,
									u32 classCode, u8 revision);
	virtual						~PciDevice() {}

			// Little endian configuration register access.
	virtual	u32					ConfigRead(u32 reg, int size, int initiator);
	virtual	void				ConfigWrite(u32 reg, u32 value, int size);

			// Declares base address register 'index' (0-5) with a power of
			// two size.
			void				DeclareBar(int index, BarType type, u32 size,
									bool prefetchable = false);
			u32					BarAddress(int index) const;
			// The expansion ROM (a power of two size).
			void				DeclareRomBar(u32 size);
			u32					RomBarAddress() const
									{ return LoadLE32(&fConfig[0x30])
										& ~(fRomSize - 1); }
			bool				RomEnabled() const
									{ return fRomSize != 0
										&& (fConfig[0x30] & 1) != 0
										&& MemoryEnabled(); }
			bool				MemoryEnabled() const
									{ return (fConfig[4] & 2) != 0; }
			bool				IoEnabled() const
									{ return (fConfig[4] & 1) != 0; }
			bool				BusMasterEnabled() const
									{ return (fConfig[4] & 4) != 0; }

			void				SetInterruptPin(u8 pin) { fConfig[0x3d] = pin; }
			void				SetDeviceNumber(int number)
									{ fDeviceNumber = number; }
			int					DeviceNumber() const { return fDeviceNumber; }
	virtual	const char*			PciName() const = 0;

protected:
			// Called when a BAR's address or the command register's enable
			// bits change; the device (re)maps its resources.
	virtual	void				BarsChanged() {}
	virtual	void				ConfigRegisterWritten(u32 reg) {}

			u8					fConfig[256];

private:
			BarType				fBarType[6];
			u32					fBarSize[6];
			u32					fRomSize;
			int					fDeviceNumber;
};

class PciBus {
public:
								PciBus();

			void				Attach(int deviceNumber, PciDevice* device);
			PciDevice*			Device(int number) const
									{ return number >= 0 && number < 32
										? fDevices[number] : nullptr; }

			// Type 0 configuration cycle on bus 0.
			u32					ConfigRead(int device, int function,
									u32 reg, int size, int initiator);
			void				ConfigWrite(int device, int function,
									u32 reg, u32 value, int size);

private:
			PciDevice*			fDevices[32];
};

}	// namespace powermac

#endif	// PM_PCI_BUS_H
