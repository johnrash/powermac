/*
 * PowerMac emulator for Haiku - shared definitions.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#ifndef PM_APP_DEFS_H
#define PM_APP_DEFS_H

#include <SupportDefs.h>

#define kAppSignature "application/x-vnd.PowerMac-Emulator"
#define kAppName "PowerMac"

enum {
	kMsgPowerOn = 'pwon',
	kMsgPowerOff = 'pwof',
	kMsgReset = 'rset',
	kMsgInterruptSwitch = 'intr',
	kMsgPowerKey = 'pkey',
	kMsgRestartHolding = 'rhld',
	kMsgSwitchProfile = 'swpf',
	kMsgNewProfile = 'nwpf',
	kMsgRenameProfile = 'rnpf',
	kMsgDeleteProfile = 'dlpf',
	kMsgPause = 'paus',
	kMsgPreferences = 'pref',
	kMsgPreferencesChanged = 'prch',
	kMsgFrame = 'fram',
	kMsgScale = 'scal',
	kMsgInsertCd = 'incd',
	kMsgCdChosen = 'cdch',
	kMsgEjectCd = 'ejcd',
	kMsgReinsertCd = 'rics',
	kMsgInsertFloppy = 'infl',
	kMsgFloppyChosen = 'flch',
	kMsgEjectFloppy = 'ejfl',
	kMsgCaptureChanged = 'capt',
	kMsgScreenshot = 'shot',
	kMsgSpeedLimit = 'sped',
	kMsgTypeKeys = 'type',
	kMsgAbout = 'abou',
	kMsgNewDiskCreated = 'ndcr',
	kMsgResetNvram = 'rsnv',
	kMsgResetPram = 'rspr',
	kMsgSerialConsole = 'scon'
};

#endif	// PM_APP_DEFS_H
