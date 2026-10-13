// lmxxf's neural rendering runtime (thirdparty/lmxxf-nr, MIT), built as
// LmxxfNrRuntime.dll: loaded by the game only when neural rendering is on
// (xenia's gpu/d3d12/dao_upscale_neural.cc).
#include <windows.h>
// rpcndr.h (through windows.h) defines `small` as char; the runtime uses
// small as a variable name.
#undef small
// The runtime's own hotkeys (F6-F8) would read the keyboard whatever window
// has focus: they're off (it never sees a key down). The game's F6 (main.cpp)
// pauses it instead, and the game's settings do the rest.
#define GetAsyncKeyState(key) SHORT(0)
#include "LmxxfNrRuntime.cpp"
