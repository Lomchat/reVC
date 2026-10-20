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
	// The mouse over the list (absolute while it is open), a click on it: a code clicked is as if typed.
	void CheatListMouse(float x, float y);
	bool CheatListClick(float x, float y);
	// The game's left button, read (Pad.cpp): true while the click that chose a code is still down.
	bool SwallowLeftButton(bool down);

	// The frontend: a page shown (SwitchToNewScreen), a game to load (MENUPAGE_LOADING_IN_PROGRESS: the slot's, or the
	// free mode's start — CheckSlotDataValid's part), a new story game; a game saved (a save screen opening, a mission's
	// quick save: C_PcSave::SaveSlot) in its own files.
	void ScreenChanged(int screen);
	bool CheckLoad(int32 slot);
	void StartingNewGame();
	void SaveMenuOpening();
	// A saved game loaded (GenericLoad): a free one's state put right.
	void GameLoaded();
	// The free mode's page.
	void FreeModeContinuePress(int8 action);
	void FreeModeNewPress(int8 action);
	void FreeModeLoadPress(int8 action);
	void FreeModeDeletePress(int8 action);
	// The shared world's base (OrthrosMmo's session, src/extras/mmo): the free mode's start loaded as a new free game is,
	// without the free mode's rules (no free saves, no pause "Save Game"); OrthrosMmo::GameLoaded prepares it. Given up
	// before its load: the start page again.
	void StartSharedWorld();
	void CancelSharedWorld();
}
#endif
