#pragma once

// Orthros' additions to the game, for its own modified Vice City (built with -DREVC_ORTHROS_EXTRAS=ON, which defines
// ORTHROS_EXTRAS; the browser build without it is reVC as it is):
//   - the HELP cheat: the cheats by kind, in the game's own fonts and colours, over the paused game;
//   - the free mode (command line -freemode): its own save files, started from the most recent of them — the first time
//     from a game completed at 100 % (orthros/freemode.b among the game's files) —, saved from the pause menu anywhere.

#ifdef ORTHROS_EXTRAS
namespace OrthrosExtras
{
	extern bool bFreeMode;

	// The free mode asked on the command line: true when `arg` was its option.
	bool CommandLine(const char *arg);
	// The save files' name before their slot number ("GTAVCsf", the free mode's "GTAVCfree").
	const char *SaveFilePrefix();

	void ToggleCheatList();
	bool IsCheatListOpen();
	// Once a frame, before the frontend: the open list's keys from a pad (the keyboard's close it at the key, events.cpp).
	void ProcessCheatList();
	// Once a frame, over everything else.
	void RenderCheatList();

	// The free mode, once the main menu is up at the start: the most recent of its saves loaded.
	void FreeModeBoot();
	// The free mode's "Save Game" in the pause menu (once the command line asked for the free mode).
	void FreeModeAddMenuEntries();
}
#endif
