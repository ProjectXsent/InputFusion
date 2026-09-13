/*
Blocks the game's non-XInput controller detection (DirectInput + Raw Input)
so it only sees gamepads through the SDL3-backed XInput proxy in xinput.cpp.

Only one of the two hooks below may actually be needed for your game -
use Process Monitor / API Monitor to see which API the game calls for
input, then feel free to delete the other hook to keep things simple.

Requires Detours (https://github.com/microsoft/Detours).
*/

#include "native_input_block.h"

#include <windows.h>
#include <dinput.h>
#include <detours.h>

#include <atomic>
#include <vector>
#include <algorithm>

#pragma comment(lib, "user32.lib")

static std::atomic<bool> g_installed{ false };

// ---------------------------------------------------------------------
// Raw Input: hide gamepad/joystick HID devices from GetRawInputDeviceList
// ---------------------------------------------------------------------
using GetRawInputDeviceList_t = UINT(WINAPI*)(PRAWINPUTDEVICELIST, PUINT, UINT);
using GetRawInputDeviceInfoW_t = UINT(WINAPI*)(HANDLE, UINT, LPVOID, PUINT);

static GetRawInputDeviceList_t  Real_GetRawInputDeviceList  = nullptr;
static GetRawInputDeviceInfoW_t Real_GetRawInputDeviceInfoW = nullptr;

static bool IsGamepadOrJoystick(HANDLE hDevice) {
    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT size = sizeof(info);

    if (Real_GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) {
        return false;
    }
    if (info.dwType != RIM_TYPEHID) return false;

    // HID Usage Page 0x01 (Generic Desktop), Usage 0x04 (Joystick) / 0x05 (Gamepad)
    return info.hid.usUsagePage == 0x01 &&
           (info.hid.usUsage == 0x04 || info.hid.usUsage == 0x05);
}

static UINT WINAPI Hook_GetRawInputDeviceList(PRAWINPUTDEVICELIST pRawInputDeviceList, PUINT puiNumDevices, UINT cbSize) {
    if (!puiNumDevices) return (UINT)-1;

    UINT fullCount = 0;
    Real_GetRawInputDeviceList(nullptr, &fullCount, cbSize);
    if (fullCount == 0) {
        *puiNumDevices = 0;
        return 0;
    }

    std::vector<RAWINPUTDEVICELIST> all(fullCount);
    UINT gotten = Real_GetRawInputDeviceList(all.data(), &fullCount, cbSize);
    if (gotten == (UINT)-1) return (UINT)-1;
    all.resize(gotten);

    std::vector<RAWINPUTDEVICELIST> filtered;
    filtered.reserve(all.size());
    for (auto& d : all) {
        if (d.dwType == RIM_TYPEHID && IsGamepadOrJoystick(d.hDevice)) {
            continue; // hide it from the game
        }
        filtered.push_back(d);
    }

    if (pRawInputDeviceList == nullptr) {
        *puiNumDevices = (UINT)filtered.size();
        return 0;
    }

    if (*puiNumDevices < filtered.size()) {
        *puiNumDevices = (UINT)filtered.size();
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }

    std::copy(filtered.begin(), filtered.end(), pRawInputDeviceList);
    *puiNumDevices = (UINT)filtered.size();
    return (UINT)filtered.size();
}

// ---------------------------------------------------------------------
// DirectInput: skip gamepad/joystick devices during EnumDevices
// ---------------------------------------------------------------------
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using EnumDevicesW_fn      = HRESULT(WINAPI*)(IDirectInput8W*, DWORD, LPDIENUMDEVICESCALLBACKW, LPVOID, DWORD);

static DirectInput8Create_t Real_DirectInput8Create = nullptr;
static EnumDevicesW_fn      Real_EnumDevicesW        = nullptr;
static bool                 g_diVtableHooked         = false;

struct FilterCtx {
    LPDIENUMDEVICESCALLBACKW real;
    LPVOID ref;
};

static BOOL CALLBACK FilteredEnumCallback(LPCDIDEVICEINSTANCEW inst, LPVOID ref) {
    auto* c = reinterpret_cast<FilterCtx*>(ref);
    BYTE devType = static_cast<BYTE>(inst->dwDevType & 0xFF);
    if (devType == DI8DEVTYPE_GAMEPAD || devType == DI8DEVTYPE_JOYSTICK) {
        return DIENUM_CONTINUE; // skip - don't forward to the game's real callback
    }
    return c->real(inst, c->ref);
}

static HRESULT WINAPI Hook_EnumDevicesW(IDirectInput8W* self, DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags) {
    FilterCtx ctx{ lpCallback, pvRef };
    return Real_EnumDevicesW(self, dwDevType, &FilteredEnumCallback, &ctx, dwFlags);
}

static HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    HRESULT hr = Real_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    if (FAILED(hr) || !ppvOut || !*ppvOut) return hr;

    if (!g_diVtableHooked) {
        auto* iface = reinterpret_cast<IDirectInput8W*>(*ppvOut);
        void** vtable = *reinterpret_cast<void***>(iface);

        // IDirectInput8::EnumDevices is vtable slot 4:
        // 0 QueryInterface, 1 AddRef, 2 Release, 3 CreateDevice, 4 EnumDevices
        const int kEnumDevicesSlot = 4;

        DWORD oldProtect;
        VirtualProtect(&vtable[kEnumDevicesSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        Real_EnumDevicesW = reinterpret_cast<EnumDevicesW_fn>(vtable[kEnumDevicesSlot]);
        vtable[kEnumDevicesSlot] = reinterpret_cast<void*>(&Hook_EnumDevicesW);
        VirtualProtect(&vtable[kEnumDevicesSlot], sizeof(void*), oldProtect, &oldProtect);

        g_diVtableHooked = true;
    }
    return hr;
}

// ---------------------------------------------------------------------
void NativeInputBlock_Install() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    // --- Resolve real addresses first; Detours needs the pointer to
    //     already hold the target address before DetourAttach.
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        Real_GetRawInputDeviceList  = reinterpret_cast<GetRawInputDeviceList_t>(
            GetProcAddress(hUser32, "GetRawInputDeviceList"));
        Real_GetRawInputDeviceInfoW = reinterpret_cast<GetRawInputDeviceInfoW_t>(
            GetProcAddress(hUser32, "GetRawInputDeviceInfoW"));
    }

    HMODULE hDinput8 = GetModuleHandleW(L"dinput8.dll");
    if (!hDinput8) hDinput8 = LoadLibraryW(L"dinput8.dll");
    if (hDinput8) {
        Real_DirectInput8Create = reinterpret_cast<DirectInput8Create_t>(
            GetProcAddress(hDinput8, "DirectInput8Create"));
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (Real_GetRawInputDeviceList && Real_GetRawInputDeviceInfoW) {
        DetourAttach(reinterpret_cast<PVOID*>(&Real_GetRawInputDeviceList), Hook_GetRawInputDeviceList);
    }
    if (Real_DirectInput8Create) {
        DetourAttach(reinterpret_cast<PVOID*>(&Real_DirectInput8Create), Hook_DirectInput8Create);
    }

    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        // Roll back on failure so Real_* pointers don't point at half-hooked trampolines.
        Real_GetRawInputDeviceList = nullptr;
        Real_DirectInput8Create = nullptr;
    }

    // The DirectInput EnumDevices hook is a vtable patch (not a Detours
    // target - Detours attaches to functions, not interface vtables), so
    // it's applied lazily the first time Hook_DirectInput8Create runs.
}
