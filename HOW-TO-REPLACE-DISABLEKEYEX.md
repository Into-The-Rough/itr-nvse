# Plan: Remove RunScriptLine calls from OnKeyStateHandler

## Goal
Replace `RunScriptLine("DisableKey ...")` and `RunScriptLine("EnableKey ...")` calls with direct `DIHookControl::SetKeyDisableState()` calls, eliminating the need to go through the console interface.

## Background
- `DisableKey`/`EnableKey` are **NVSE core commands** (not Stewie/JIP)
- They use `DIHookControl::SetKeyDisableState(keycode, bDisable, mask)`
- NVSE exposes `DIHookControl*` via `NVSEDataInterface::GetSingleton(kNVSEData_DIHookControl)`

## Implementation

### 1. Add DIHookControl interface to OnKeyStateHandler.cpp

```cpp
//minimal DIHookControl interface
class DIHookControl {
public:
    void SetKeyDisableState(UInt32 keycode, bool bDisable, UInt32 mask);
};

enum {
    kDisable_User = 1 << 0,
    kDisable_Script = 1 << 1,
    kDisable_All = kDisable_User | kDisable_Script,
};

static DIHookControl* g_diHookCtrl = nullptr;
```

### 2. Get DIHookControl pointer in OKSH_Init

```cpp
//get data interface
NVSEDataInterface* dataInterface = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data);
if (dataInterface) {
    g_diHookCtrl = (DIHookControl*)dataInterface->GetSingleton(1); //kNVSEData_DIHookControl = 1
}
```

### 3. Replace RunScriptLine calls

**In Cmd_DisableKeyEx_Execute (~line 450):**
```cpp
//before:
if (g_okshConsole) {
    g_okshConsole->RunScriptLine(cmd, nullptr);
}

//after:
if (g_diHookCtrl) {
    g_diHookCtrl->SetKeyDisableState(keycode, true, mask ? mask : 3);
}
```

**In Cmd_EnableKeyEx_Execute (~line 495):**
```cpp
//before:
if (g_okshConsole) {
    g_okshConsole->RunScriptLine(cmd, nullptr);
}

//after:
if (g_diHookCtrl) {
    g_diHookCtrl->SetKeyDisableState(keycode, false, mask ? mask : 3);
}
```

### 4. Clean up unused code
- Remove `g_okshConsole` variable and console interface query (if not used elsewhere)
- Remove `NVSEConsoleInterface` struct definition

## Files to modify
- `/mnt/d/plugins/ITR-NVSE/itr-nvse/OnKeyStateHandler.cpp`

## Verification
1. Build the plugin
2. Test in-game:
   - Call `DisableKeyEx 42` (G key) from console or script
   - Verify G key no longer works
   - Call `EnableKeyEx 42`
   - Verify G key works again
3. Verify event callbacks still fire
