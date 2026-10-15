/*
 * PowerMac emulator - a DBDMA channel (Apple's descriptor based DMA).
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Registers (little endian 32 bits, 0x100 per channel in Grand Central's
 * window at 0x8000):
 *   0x00 control: (mask << 16) | value for the writable status bits
 *   0x04 status: RUN 0x8000, PAUSE 0x4000, FLUSH 0x2000, WAKE 0x1000,
 *        DEAD 0x800, ACTIVE 0x400, BT 0x100 (branch taken), S7-S0 (the
 *        device's and the software's condition bits)
 *   0x0C command pointer (16 byte aligned; written while not ACTIVE)
 *   0x10, 0x14, 0x18 interrupt, branch, wait select: (mask << 16) |
 *        value; the condition is (S & mask) == (value & mask)
 *
 * A command descriptor (16 bytes, little endian): request count (16),
 * command (16: 15-12 the command, 10-8 the key, 5-4 interrupt, 3-2
 * branch, 1-0 wait: 0 never, 1 if the condition, 2 if not, 3 always),
 * data address (32), command dependent (32: branch address, quad data),
 * residual count (16) and transfer status (16), the last two written back
 * at the end.
 *
 * Commands: OUTPUT_MORE/LAST (memory to device), INPUT_MORE/LAST (device
 * to memory), STORE_QUAD and LOAD_QUAD (1, 2 or 4 bytes between the
 * command dependent field and any address, key 6), NOP, STOP (the channel
 * idles, still RUN, until software sets WAKE or RUN again: then it fetches
 * the command at the command pointer; a device's Kick() does not wake it).
 *
 * The device moves the data at its pace: a transfer offers it what is
 * left; when it takes or gives nothing the channel waits until the device
 * calls Kick(). A device ends an input early (a short SCSI read) with
 * EndInput().
 */
#ifndef PM_DBDMA_H
#define PM_DBDMA_H

#include "../bus/PhysBus.h"
#include "../Types.h"

#include <functional>

namespace powermac {

class DbdmaChannel : public MmioDevice {
public:
	enum {
		kRun = 0x8000,
		kPause = 0x4000,
		kFlush = 0x2000,
		kWake = 0x1000,
		kDead = 0x0800,
		kActive = 0x0400,
		kBranchTaken = 0x0100
	};

	class Device {
	public:
		virtual ~Device() {}
		// Memory to device: takes up to 'length' bytes, returns how many.
		virtual u32 DmaOutput(const u8* data, u32 length) { return 0; }
		// Device to memory: gives up to 'length' bytes, returns how many.
		virtual u32 DmaInput(u8* data, u32 length) { return 0; }
		// An OUTPUT_LAST descriptor has handed over all its data (the end
		// of a frame for the Ethernet controller).
		virtual void DmaOutputLast() {}
	};

								DbdmaChannel(PhysBus& bus, const char* name);

			void				Reset();
			void				SetDevice(Device* device)
									{ fDevice = device; }
			void				SetInterruptCallback(
									std::function<void(bool)> callback)
									{ fInterrupt = callback; }

			// The device can move data again.
			void				Kick();
			// The device has no more input for the descriptor in progress.
			void				EndInput();
			// The device's condition bits (S7-S0, under 'mask').
			void				SetDeviceStatus(u8 mask, u8 bits);

			bool				IsRunning() const
									{ return (fStatus & kRun) != 0; }
			bool				IsActive() const
									{ return (fStatus & kActive) != 0; }

	// MmioDevice: offsets within the channel's 0x100 bytes
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return fName; }

private:
	struct Descriptor {
		u16 requestCount;
		u16 command;
		u32 address;
		u32 dependent;
	};

			bool				_Condition(u32 select) const;
			bool				_Test(int field, u32 select) const;
			void				_Process();
			bool				_Fetch();
			// Finishes the descriptor in progress: write back, interrupt,
			// branch; false when the channel stops there.
			bool				_Complete();
			void				_Quad(bool load);
			void				_Pulse();
			u32					_Read32(u32 offset) const;
			void				_Write32(u32 offset, u32 value);

			PhysBus&			fBus;
			const char*			fName;
			Device*				fDevice;
			std::function<void(bool)> fInterrupt;

			u32					fStatus;
			u32					fCommandPointer;
			u32					fInterruptSelect;
			u32					fBranchSelect;
			u32					fWaitSelect;

			bool				fHaveDescriptor;
			Descriptor			fDescriptor;
			u32					fDone;			// bytes moved so far
			bool				fInputEnded;
			bool				fWaiting;		// the wait condition holds
			bool				fStopped;		// at a STOP: until WAKE
			bool				fProcessing;	// _Process() is running
};

}	// namespace powermac

#endif	// PM_DBDMA_H
