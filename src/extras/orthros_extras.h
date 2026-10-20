#pragma once

// Orthros' additions to the game, for its own modified Vice City (built with -DREVC_ORTHROS_EXTRAS=ON, which defines
// ORTHROS_EXTRAS; the browser build without it is reVC as it is):
//   - the HELP cheat: the cheats by kind, in the game's own fonts and colours, over the paused game;
//   - the free mode, a game of its own chosen in the main menu (its page, MENUPAGE_FREE_MODE): its own save files,
//     "Continue" from the most recent, "New Free Game" from a game completed at 100 % among the game's files
//     (orthros/freemode.b), saved from the pause menu anywhere.

#ifdef ORTHROS_EXTRAS
#include "common.h"

namespace OrthrosExtras
{
	// The game being played is a free one.
	extern bool bFreeMode;

	// The save files' name before their slot number ("GTAVCsf", the free mode's "GTAVCfree"): the ones the game's lists
	// and saves use now.
	const char *SaveFilePrefix();
	bool UsingFreeSaves();
	// The texts the game's text files lack ("ORT_*" keys: CText::Get asks here first); nil for any other key.
	wchar *Text(const char *key);

	void ToggleCheatList();
	bool IsCheatListOpen();
	// Once a frame, before the frontend: the open list's keys from a pad (the keyboard's close it at the key, events.cpp).
	void ProcessCheatList();
	// Once a frame, over everything else.
	void RenderCheatList();

	// The frontend: a page shown (SwitchToNewScreen), a game to load (MENUPAGE_LOADING_IN_PROGRESS: the slot's, or the
	// free mode's start — CheckSlotDataValid's part), a new story game; a game saved (a save screen opening, a mission's
	// quick save: C_PcSave::SaveSlot) in its own files.
	void ScreenChanged(int screen);
	bool CheckLoad(int32 slot);
	void StartingNewGame();
	void SaveMenuOpening();
	// The free mode's page.
	void FreeModeContinuePress(int8 action);
	void FreeModeNewPress(int8 action);
	void FreeModeLoadPress(int8 action);
	void FreeModeDeletePress(int8 action);
}
#endif
