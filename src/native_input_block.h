#pragma once

// Blocks the game's OWN DirectInput / Raw Input gamepad detection so it
// only ever sees controllers through the SDL3-backed XInput proxy in
// xinput.cpp. Fixes "double input" where the game reads the stick/buttons
// once through its legacy joystick code (DirectInput or Raw Input) and
// again through the fake XInput device this DLL provides.
//
// Call this ONCE, as early as possible - ideally from DllMain on
// DLL_PROCESS_ATTACH, before the game has a chance to create its
// DirectInput device or register for raw input. If the game creates its
// input devices very early (before your DLL is even loaded, e.g. it's
// statically imported), call it from the first XInputGetStateEx/
// XInputGetState invocation instead - later is still better than never,
// since most engines re-enumerate periodically or on device-changed
// notifications.
//
// Requires MinHook: https://github.com/TsudaKageyu/minhook
//   vcpkg install minhook
//   link against minhook (libMinHook.x64.lib / libMinHook.x86.lib)
void NativeInputBlock_Install();
