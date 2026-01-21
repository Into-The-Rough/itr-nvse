# itr-nvse Handover - QuickDrop/Quick180 Hook Issue

## Project Overview
itr-nvse is a consolidated NVSE plugin for Fallout New Vegas that merges multiple small plugins into one. Located at `D:\plugins\itr-nvse`.

## Working Features
- **DialogueTextFilter** - Works (isolated module with JIP-LN-NVSE headers)
- **MessageBoxQuickClose** - Works (isolated module with JIP-LN-NVSE headers)
- **ConsoleLogCleaner** - Works (deletes consoleout.txt on startup)
- **AltTabMute** - Works (mutes audio when alt-tabbing)
- **AutoGodMode** - Works
- **AutoQuickLoad** - Works

## BROKEN: QuickDrop & Quick180

### The Problem
Porting `QuickDropNVSE` and `Quick180NVSE` from `D:\plugins\QuickDropNVSE` and `D:\plugins\Quick180NVSE` into itr-nvse. The original standalone plugins work perfectly. The port does not work at all - the hook is never called.

### What We Know
1. **Hook is being installed** - Log shows patch is applied:
   ```
   PlayerUpdateHook: Original target at 0x009673D0
   PlayerUpdateHook: New target at 0x0DFB2056 (hook at 0x0DFB2056)
   ```
2. **Hook is never called** - No beeps, no log output from inside the hook, nothing happens
3. **Original plugins work** - When enabled separately, QuickDropNVSE.dll and Quick180NVSE.dll work fine
4. **Code is identical** - The hook code was copied exactly from the originals

### Original Plugin Code (WORKING)
Location: `D:\plugins\QuickDropNVSE\main.cpp` and `D:\plugins\Quick180NVSE\main.cpp`

Both plugins:
- Use `<cstdint>` and `<Windows.h>` only (no NVSE headers)
- Hook address `0x940C78` (a CALL instruction in PlayerCharacter::Update)
- Use `__fastcall` hook with signature: `void __fastcall PlayerUpdate_Hook(void* player, void* edx, float timeDelta)`
- Call original via: `((void(__thiscall*)(void*, float))g_originalCallTarget)(player, timeDelta)`

### Current Implementation (BROKEN)
In `main.cpp`, namespace `PlayerUpdateHook` contains the combined hook. Also user created `PlayerInputHooks.cpp` as an isolated module attempt (check if this exists and is being used).

### What Was Tried
1. Combined both features into single hook - doesn't work
2. Separated into two namespaces with two hooks - doesn't work
3. Added debug beeps and logging - hook never executes
4. Verified bytes at call site change after patching - they do
5. Using exact `uint32_t` types like originals - still broken

### Key Files
- `D:\plugins\itr-nvse\itr-nvse\main.cpp` - Main plugin with PlayerUpdateHook namespace (duplicate code, may conflict)
- `D:\plugins\itr-nvse\itr-nvse\PlayerInputHooks.cpp` - Isolated module attempt (see below)
- `D:\plugins\itr-nvse\itr-nvse\internal\settings.h` - Settings for bQuickDrop, bQuick180, etc.
- `D:\plugins\QuickDropNVSE\main.cpp` - WORKING original
- `D:\plugins\Quick180NVSE\main.cpp` - WORKING original

### PlayerInputHooks.cpp (Isolated Module Attempt)
User created this file with:
- Only `<cstdint>` and `<Windows.h>` includes (no NVSE headers)
- Forward declares Settings namespace variables
- Exposes `PIH_Init()` function
- **NOT YET IN VCXPROJ** - needs to be added to build
- **NOT CALLED FROM MAIN** - main.cpp still has duplicate PlayerUpdateHook namespace and calls `PlayerUpdateHook::Init()`

To properly use this isolated module:
1. Add `PlayerInputHooks.cpp` to vcxproj (WITHOUT forced include of nvse/prefix.h)
2. Create `PlayerInputHooks.h` with `bool PIH_Init();`
3. Remove duplicate `PlayerUpdateHook` namespace from main.cpp
4. Call `PIH_Init()` instead of `PlayerUpdateHook::Init()`

### INI Settings
```ini
[Tweaks]
bQuickDrop=1
bQuick180=1

[QuickDrop]
iModifierKey=16  ; VK_SHIFT
iControlID=7     ; Ready Weapon

[Quick180]
iModifierKey=16  ; VK_SHIFT
iControlID=5     ; Run/Activate
```

### Theories to Investigate
1. **Compilation difference** - Original plugins compile standalone with minimal headers. itr-nvse includes full NVSE headers. Maybe something in the NVSE headers conflicts?
2. **Module isolation** - MessageBoxQuickClose works because it's in a separate .cpp with different headers. Maybe QuickDrop/Quick180 need the same treatment?
3. **Hook timing** - Maybe another plugin or the NVSE headers install something at the same address after us?
4. **Calling convention issue** - Something about how the function is compiled in the context of the larger plugin?

### Suggested Next Steps
1. **Check PlayerInputHooks.cpp** - User created this file, see if it's a separate module approach
2. **Create isolated module** - Like MessageBoxQuickClose, make `PlayerInputHooks.cpp` that compiles WITHOUT nvse/prefix.h forced include
3. **Compare compiled code** - Disassemble working QuickDropNVSE.dll vs itr-nvse.dll hook function
4. **Try absolute minimal test** - Create a hook that just does `Beep(750,100)` every frame, nothing else

### Build Command
```
"/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" itr-nvse.sln -p:Configuration=Release -p:Platform=Win32 -verbosity:minimal
```

### Log Location
`D:\Modding\VNV\overwrite\NVSE\plugins\itr-nvse.log`

---

## Reference: Original Working QuickDropNVSE (COMPLETE SOURCE)

```cpp
#include <cstdint>
#include <Windows.h>

#define EXTERN_DLL_EXPORT extern "C" __declspec(dllexport)

constexpr char const* PLUGIN_NAME = "QuickDropNVSE";
constexpr uint32_t PLUGIN_VERSION = 1;

struct PluginInfo {
    enum { kInfoVersion = 1 };
    uint32_t infoVersion;
    const char* name;
    uint32_t version;
};

struct NVSEInterface {
    uint32_t nvseVersion;
    uint32_t runtimeVersion;
    uint32_t editorVersion;
    uint32_t isEditor;
};

namespace QuickDrop {
    constexpr uint32_t kAddr_OSGlobals = 0x11DEA0C;
    constexpr uint32_t kAddr_OSInputGlobals = 0x11F35CC;
    constexpr uint32_t kAddr_GetControlState = 0xA24660;
    constexpr uint32_t kAddr_PlayerUpdateCall = 0x940C78;
    constexpr uint32_t kAddr_TryDropWeapon = 0x89F580;
    constexpr uint32_t kAddr_GetEquippedWeapon = 0x8A1710;
    constexpr uint32_t kOffset_OSGlobals_Window = 0x08;

    enum KeyState { isHeld, isPressed, isDepressed, isChanged };

    char g_iniPath[MAX_PATH];
    int g_iModifierKey = VK_SHIFT;
    int g_iControlID = 7;
    bool g_lastFramePressed = false;
    uint32_t g_originalCallTarget = 0;

    void LoadConfig() {
        g_iModifierKey = GetPrivateProfileIntA("Keybinds", "iModifierKey", VK_SHIFT, g_iniPath);
        g_iControlID = GetPrivateProfileIntA("Keybinds", "iControlID", 7, g_iniPath);
    }

    void SafeWrite32(uint32_t addr, uint32_t data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(uint32_t*)addr = data;
        VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
    }

    void ReplaceCall(uint32_t jumpSrc, void* jumpTgt) {
        SafeWrite32(jumpSrc + 1, (uint32_t)jumpTgt - jumpSrc - 5);
    }

    uint32_t GetRelJumpAddr(uint32_t jumpSrc) {
        return *(uint32_t*)(jumpSrc + 1) + jumpSrc + 5;
    }

    bool GetControlState(void* input, uint32_t controlCode, KeyState state) {
        return ((bool(__thiscall*)(void*, uint32_t, KeyState))kAddr_GetControlState)(input, controlCode, state);
    }

    void* GetEquippedWeapon(void* actor) {
        return ((void*(__thiscall*)(void*))kAddr_GetEquippedWeapon)(actor);
    }

    void TryDropWeapon(void* actor) {
        ((void(__thiscall*)(void*))kAddr_TryDropWeapon)(actor);
    }

    void __fastcall PlayerUpdate_Hook(void* player, void* edx, float timeDelta) {
        ((void(__thiscall*)(void*, float))g_originalCallTarget)(player, timeDelta);

        void* osGlobals = *(void**)kAddr_OSGlobals;
        void* inputGlobals = *(void**)kAddr_OSInputGlobals;

        if (!osGlobals) {
            g_lastFramePressed = false;
            return;
        }

        HWND gameWindow = *(HWND*)((uint8_t*)osGlobals + kOffset_OSGlobals_Window);
        if (GetForegroundWindow() != gameWindow) {
            g_lastFramePressed = false;
            return;
        }

        bool modifierHeld = (g_iModifierKey == 0) || ((GetAsyncKeyState(g_iModifierKey) & 0x8000) != 0);
        bool controlPressed = GetControlState(inputGlobals, g_iControlID, isPressed);

        if (controlPressed && !g_lastFramePressed && modifierHeld) {
            if (GetEquippedWeapon(player)) {
                TryDropWeapon(player);
            }
        }

        g_lastFramePressed = controlPressed;
    }

    void InitHooks() {
        g_originalCallTarget = GetRelJumpAddr(kAddr_PlayerUpdateCall);
        ReplaceCall(kAddr_PlayerUpdateCall, PlayerUpdate_Hook);
    }
}

EXTERN_DLL_EXPORT bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = PLUGIN_NAME;
    info->version = PLUGIN_VERSION;
    return !nvse->isEditor;
}

EXTERN_DLL_EXPORT bool NVSEPlugin_Load(NVSEInterface* nvse) {
    using namespace QuickDrop;
    GetModuleFileNameA(GetModuleHandleA("QuickDropNVSE.dll"), g_iniPath, MAX_PATH);
    char* lastSlash = strrchr(g_iniPath, '\\');
    if (lastSlash) strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - g_iniPath), "QuickDropNVSE.ini");
    LoadConfig();
    InitHooks();
    return true;
}

BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved) {
    return TRUE;
}
```

The key difference: this plugin is COMPLETELY STANDALONE with no NVSE header includes. It defines its own minimal PluginInfo and NVSEInterface structs.
