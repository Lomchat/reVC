#pragma once

// Orthros' additions to the game, for its own modified Vice City (built with -DREVC_ORTHROS_EXTRAS=ON, which defines
// ORTHROS_EXTRAS; the browser build without it is reVC as it is):
//   - the HELP cheat: the cheats by kind, in the game's own fonts and colours, over the paused game (its list drawn
//     for other lists too: the shared world's teleport list);
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
	// UTF-8 (or Latin-1) to the game's text: the accented letters at the codes its fonts use, '?' for what they lack.
	void GameText(const char *utf8, wchar *out, int size);

	// The cheats HELP lists, every keyboard code of the game (the shared world's gate reads them): their number, a code
	// ("ASPIRINE", nil past the last) and its kind (CHEAT_*, the list's groups).
	enum { CHEAT_WEAPONS, CHEAT_POLICE, CHEAT_VEHICLES, CHEAT_TRAFFIC, CHEAT_WEATHER, CHEAT_PEDS, CHEAT_TOMMY, CHEAT_OTHER };
	int NumCheats();
	const char *CheatCode(int i);
	int CheatKind(int i);

	// A list over the game, as HELP's: groups of rows, each a name (white, left) and a description (pink, right), in
	// columns, in the game's own fonts and colours; the row under the mouse highlighted (or, when the list takes the
	// keys, the one chosen with the arrows); a row clicked (or Enter on it) is chosen. One list is open at a time; the
	// game pauses under it, except in a shared world (OrthrosMmo::ListsPause): the player's controls are then off while
	// it is open (PLAYERCONTROL_ORTHROS_LIST).
	enum { PLAYERCONTROL_ORTHROS_LIST = 0x4000 };
	struct List
	{
		const char *title[2], *footer[2]; // UTF-8: English, French
		int numGroups;
		bool keys;            // the arrows and Enter choose a row (else Esc, Enter or Backspace close the list, as HELP)
		float scale;          // its texts' and rows' size (HELP's: 1)
		float maxColumnWidth; // in the game's 640-wide screen units (0: the screen's share)
		int (*group)(int group, wchar *heading, int size);                   // a group's heading; its number of rows
		void (*row)(int group, int row, wchar *name, wchar *desc, int size); // a row's texts
		void (*chosen)(int group, int row);                                  // a row chosen (the list closed already)
	};
	void OpenList(const List *list);
	void CloseList();
	bool IsListOpen(const List *list);
	// A key pressed (events.cpp HandleKeyDown, rsKeyCodes): true = the open list's, not the game's.
	bool ListKeyDown(int32 key);

	// HELP's list opened or closed.
	void ToggleCheatList();
	// A list is open (HELP's or another).
	bool IsCheatListOpen();
	// Once a frame, before the frontend: the open list's keys from a pad (the keyboard's are taken at the key, events.cpp).
	void ProcessCheatList();
	// Once a frame, over everything else: the open list.
	void RenderCheatList();
	// The mouse over the open list (absolute while it is open), a click on it: a row clicked is chosen (a cheat's code:
	// as if typed).
	void CheatListMouse(float x, float y);
	bool CheatListClick(float x, float y);
	// The game's left button, read (Pad.cpp): true while the click that chose a row is still down.
	bool SwallowLeftButton(bool down);
	// A click another overlay took (the shared world's lists): not the game's either, until it is released.
	void SwallowLeftClick();

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
