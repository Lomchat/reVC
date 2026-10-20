#pragma once

// Orthros (the browser): the game's main loop and its long loads give the browser its turn. The engine runs on the
// page's thread; with JSPI its WebAssembly stack is suspended while the page handles events, paints and fetches,
// then resumed where it stopped.
#ifdef __EMSCRIPTEN__
#ifdef __cplusplus
extern "C" {
#endif
// Once per main loop iteration: wait for the browser's next frame (the swap of a vsynced window).
void OrthrosFrameYield(void);
// During a load: the browser's turn, at most every 50 ms.
void OrthrosLoadingYield(void);
// The game takes the mouse (relative motion: the page captures it) or frees it (its menus, its own cursor drawn).
void OrthrosMouseMode(int relative);
// The size the page gives the game (its canvas' box, CSS pixels): the window's, whatever video mode the game chose.
void OrthrosCanvasSize(int *width, int *height);
// The bytes [position, position + length) of the file open as `fd` are here: 1; else 0, the page fetching them in the
// background meanwhile (a file the page streams: src/ports/goldsrc/files.js, asyncReads; any other is always here).
int OrthrosFileReady(int fd, double position, int length);
// Wait, suspended (JSPI), for those bytes.
void OrthrosFileWait(int fd, double position, int length);
// The file open as `fd` is one the game reads at random while it plays (a streaming archive, a sound bank): the page
// loads the whole of it in the background, after what the game reads first.
void OrthrosFileComplete(int fd);
#ifdef __cplusplus
}
#endif
#endif
