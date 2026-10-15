/*
 * PowerMac emulator for Haiku - preferences.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * A sidebar of sections (with the icons of Haiku's own preferences) and
 * a page per section. OK applies the changes; changes to the hardware
 * restart the machine.
 */
#ifndef PM_PREFS_WINDOW_H
#define PM_PREFS_WINDOW_H

#include "Settings.h"

#include <Messenger.h>
#include <Window.h>

class BCardLayout;
class BCheckBox;
class BFilePanel;
class BListView;
class BMenuField;
class BPopUpMenu;
class BSlider;
class BTextControl;
class BView;

class PrefsWindow : public BWindow {
public:
								PrefsWindow(Settings& settings,
									BMessenger target);
	virtual						~PrefsWindow();

	virtual	void				MessageReceived(BMessage* message);

private:
			BView*				_MachinePage();
			BView*				_RomPage();
			BView*				_DisplayPage();
			BView*				_StoragePage();
			BView*				_SoundPage();
			BView*				_SerialPage();
			BView*				_InputPage();
			BView*				_NetworkPage();
			BView*				_AdvancedPage();
			void				_AddSection(const char* name,
									const char* iconSignature, BView* page);

			BMenuField*			_PopUp(const char* label,
									const char* const* items,
									const int32* values, int count,
									int32 current, BPopUpMenu** menu,
									uint32 what = 'nop ');
			int32				_Selected(BPopUpMenu* menu) const;
			void				_Browse(int32 which, bool directory);
			// The SCSI target fields of a browse slot (null if it is none).
			bool				_ScsiSlot(int32 which, BPopUpMenu*& kind,
									BTextControl*& image);
			void				_UpdateSerialHints();
			void				_Apply();

			Settings&			fSettings;
			BMessenger			fTarget;
			BListView*			fSections;
			BCardLayout*		fPages;

			BPopUpMenu*			fModel;
			BPopUpMenu*			fMemory;
			BPopUpMenu*			fCpus;
			BPopUpMenu*			fSpeed;
			BTextControl*		fRom;
			BTextControl*		fCardRom;
			BTextControl*		fClock;
			BPopUpMenu*			fVram;
			BPopUpMenu*			fMonitor;
			// [0]: internal bus (MESH), [1]: external (53C94)
			BPopUpMenu*			fScsiKind[2][7];
			BTextControl*		fScsi[2][7];
			BCheckBox*			fScsiReadOnly[2][7];
			BTextControl*		fFloppy;
			BCheckBox*			fFloppyProtected;
			BCheckBox*			fSound;
			BSlider*			fVolume;
			BPopUpMenu*			fSerialKind[2];
			BTextControl*		fSerial[2];
			BPopUpMenu*			fHostKey;
			BCheckBox*			fEthernet;
			BTextControl*		fMacAddress;
			BTextControl*		fPortForwards;
			BSlider*			fMouseSpeed;
			BCheckBox*			fCaptureOnClick;
			BPopUpMenu*			fQuantum;
			BPopUpMenu*			fCpi;
			BCheckBox*			fThreads;
			BFilePanel*			fPanel;
			int32				fBrowsing;
};

#endif	// PM_PREFS_WINDOW_H
