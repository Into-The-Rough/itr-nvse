# OnWeaponJam Handler - Handover Document

## Goal
Implement `SetOnWeaponJamEventHandler` - a callback that fires when weapons jam due to low condition. Should pass the actor and weapon to the callback script.

## File Locations

| File | Path |
|------|------|
| **Source code** | `/mnt/d/plugins/itr-nvse/itr-nvse/OnWeaponJamHandler.cpp` |
| **Header** | `/mnt/d/plugins/itr-nvse/itr-nvse/OnWeaponJamHandler.h` |
| **Handler log** | `D:\Modding\VNV\overwrite\NVSE\plugins\OnWeaponJamHandler.log` |
| **Crash logger** | `D:\SteamLibrary\steamapps\common\Fallout New Vegas\CrashLogger.log` |
| **Project file** | `/mnt/d/plugins/itr-nvse/itr-nvse/itr-nvse.vcxproj` |

## Correct Addresses (VERIFIED)

| Item | Address | Decimal | Notes |
|------|---------|---------|-------|
| `Actor::SetAnimAction` | `0x8A73E0` | 9073632 | **CORRECT** |
| Hook call site | `0x894081` | 8994945 | Call to SetAnimAction with action=9 in FiresWeapon |
| `Actor::FiresWeapon` | `0x893A40` | 8993344 | Contains the jam logic |
| `PlayerCharacter singleton` | `0x011DEA3C` | 18738748 | |

## Technical Details

### Hook Target
- **Address**: `0x894081` - Call to `SetAnimAction(this, 9, sequence)` in `Actor::FiresWeapon_893A40`
- **Original function**: `SetAnimAction` at `0x8A7360`
- **Action 9** = weapon jam animation
- **Calling convention**: `__thiscall` (ecx = Actor*)

### SetAnimAction Prologue (at 0x8A7360)
```asm
0x8A7360: 55           push ebp
0x8A7361: 8B EC        mov ebp, esp
0x8A7363: 83 EC 20     sub esp, 20h      <- CRASH HAPPENS HERE
0x8A7366: 89 4D F0     mov [ebp-10h], ecx
0x8A7369: 8B 4D F0     mov ecx, [ebp-10h]
0x8A736C: E8 ...       call Actor::GetAnimation
```

### Game Settings for Testing
Weapon jam is DISABLED by default. To enable for testing:
```
SetGameSetting fWeaponConditionJam0 0.99
SetGameSetting fWeaponConditionJam1 0.9
SetGameSetting fWeaponConditionJam2 0.8
```
Then damage weapon condition to trigger jams. **Revolvers cannot jam** - use semi-auto weapons like 9mm pistol.

---

## What Was Tried (Chronological)

### 1. Basic hook with dispatch (WRONG ADDRESS)
- Used `WriteRelCall` to replace call at 0x894081
- Jumped to `0x8A6DE0` (WRONG - this is EvaluatePackage, not SetAnimAction)
- **Result**: Crash with garbage ecx

### 2. Found correct SetAnimAction address
- Searched IDA functions list, found `Actor::SetAnimAction` at `0x8A7360`
- Verified via xrefs that 0x894081 does call 0x8A7360
- Updated code to use correct address

### 3. __fastcall wrapper function
```cpp
static void __fastcall Hook_SetAnimAction_Jam(Actor* actor, void* edx, int action, void* sequence)
{
    g_jamActor = actor;
    DispatchWeaponJamEvent();
    OriginalSetAnimAction(actor, action, sequence);  // typedef __thiscall
}
```
- **Result**: Crash - ecx was garbage (0x000000D3) when calling original
- **Cause**: MSVC doesn't handle __thiscall function pointers correctly

### 4. __fastcall with inline asm for original call
```cpp
static void __fastcall Hook_SetAnimAction_Jam(Actor* actor, void* edx, int action, void* sequence)
{
    g_jamActor = actor;
    DispatchWeaponJamEvent();
    __asm {
        push sequence
        push action
        mov ecx, actor
        mov eax, 0x8A7360
        call eax
    }
}
```
- **Result**: Crash - ecx still garbage (0x3DEB0DF1)
- **Cause**: Inside a C++ function, inline asm reads `actor` from wrong stack location after function calls modify the stack

### 5. Save to statics before calls
```cpp
static void __fastcall Hook_SetAnimAction_Jam(Actor* actor, void* edx, int action, void* sequence)
{
    g_jamActor = actor;
    g_jamAction = action;
    g_jamSequence = sequence;  // Save FIRST

    DispatchWeaponJamEvent();

    __asm {
        push g_jamSequence
        push g_jamAction
        mov ecx, g_jamActor
        mov eax, 0x8A7360
        call eax
    }
}
```
- **Result**: Still crash with garbage ecx
- **Cause**: Compiler still interferes with inline asm in non-naked functions

### 6. Fully naked function with jmp (CURRENT)
```cpp
static __declspec(naked) void Hook_SetAnimAction_Jam()
{
    __asm {
        mov g_jamActor, ecx      // Save actor
        pushad                    // Save ALL registers
        pushfd
        call DispatchWeaponJamEvent
        popfd
        popad                     // Restore ALL registers (including ecx)
        mov eax, 0x8A7360
        jmp eax                   // Jump, not call - original's ret goes to real caller
    }
}
```
- **Result**: ecx IS NOW CORRECT! But crash moved earlier - now at `sub esp, 0x20` (SetAnimAction+3)
- **Progress**: Dispatch runs successfully, returns, then crashes in SetAnimAction prologue

---

## Current Crash (Latest)

```
Exception: EXCEPTION_ACCESS_VIOLATION (C0000005)
eip = 0x008A7363  (SetAnimAction + 3 = "sub esp, 20h")
ecx = 0x4F281780  (CORRECT - Character x3dnpcArtSeagerREF)
esi = 0x00010F80  (suspicious value)
esp = 0x0019F8B0  (looks valid)

Calltrace:
0x008A7363  <- Crash here (no return to our DLL - jmp worked!)
0x008BA7C4
...
```

Handler log shows dispatch completed:
```
>>> DispatchWeaponJamEvent ENTERED
    Checking if player: g_jamActor=0x4F281780, *g_thePlayer=0x187AED94
    NOT player, skipping (NPC jam)
<<< DispatchWeaponJamEvent EXITING (not player)
```

---

## ROOT CAUSE FOUND

**Hex/decimal conversion error!**

When searching IDA for SetAnimAction, I found address `9073632` (decimal). I incorrectly assumed this was `0x8A7360`, but:
```
9073632 decimal = 0x8A73E0 (CORRECT)
9073504 decimal = 0x8A7360 (WRONG - what I was using)
```

The code was jumping 128 bytes (0x80) into the wrong location, landing in the middle of unrelated code.

**Fix**: Change `0x8A7360` to `0x8A73E0` everywhere.

**Lesson**: ALWAYS verify hex/decimal conversions with a calculator. The CLAUDE.md even warns about this!

---

## WORKING SOLUTION

```cpp
static UInt32 s_SetAnimActionAddr = 0x8A73E0;  // CORRECT!

static __declspec(naked) void Hook_SetAnimAction_Jam()
{
    __asm {
        mov g_jamActor, ecx
        pushad
        pushfd
        call DispatchWeaponJamEvent
        popfd
        popad
        jmp dword ptr [s_SetAnimActionAddr]
    }
}
```

---

## What Was Tried (Historical)

1. **Hook INSIDE SetAnimAction** instead of at call site
   - Patch SetAnimAction's prologue directly
   - Might avoid whatever call-site issue we're hitting

2. **Use a trampoline** instead of WriteRelCall
   - Create a proper trampoline that copies original bytes
   - More robust than call-site patching

3. **Check what registers the caller preserves**
   - Maybe FiresWeapon expects certain registers to be preserved across the call
   - Our pushad/popad should handle this, but maybe not correctly

4. **Thread safety investigation**
   - Crashes always happen on NPCs (AI thread?)
   - Maybe need critical section or thread-local storage

5. **Look at other NVSE plugins**
   - Check kNVSE, JIP LN, ShowOff for animation hooks
   - See how they handle similar hooking scenarios

6. **Hook at a different point**
   - Maybe hook the RandomBool check that decides if jam happens
   - Or hook at the animation level instead of SetAnimAction

7. **Don't dispatch at all for NPCs**
   - Currently we dispatch but skip callbacks for NPCs
   - Maybe skip the entire hook path for non-players?

---

## Build Command
```bash
"/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" "D:\\plugins\\itr-nvse\\itr-nvse.sln" -p:Configuration=Release -p:Platform=Win32 -verbosity:minimal
```

DLL copies to: `D:\Modding\VNV\mods\itr-nvse\NVSE\Plugins\itr-nvse.dll`
