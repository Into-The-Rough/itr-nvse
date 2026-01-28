# SwitchWeaponOnEmpty - Technical Investigation Report

## Goal
When an NPC's weapon clip is empty, switch to another ranged weapon with ammo instead of reloading.
"Pirate with multiple pistols" - fire weapon A, switch to weapon B, fire immediately.

## What DOES NOT Work (Confirmed)

### 1. Direct EquipItem Call
```cpp
Actor::EquipItem(actor, newWeapon, 1, nullptr, true, false, true);
```
**Result:** Weapon does NOT change. GetEquippedWeapon returns same weapon before and after.

### 2. Unequip Then Equip
```cpp
Actor::UnequipItem(actor, currentWeapon, 1, nullptr, false, false, true);
Actor::EquipItem(actor, newWeapon, 1, nullptr, true, false, true);
```
**Result:** Still doesn't work.

### 3. Different EquipItem Parameters
Tried various combinations of the bool parameters - none work.

### 4. Waiting for Idle + Holstered State
Wait for `GetAnimAction() == -1` and `GetIsWeaponOut() == false`, then EquipItem.
**Result:** STILL doesn't change weapon.

### 5. CombatProcedureSwitchWeapon + AddBackgroundProcedure
```cpp
void* proc = GameHeapAlloc(0x18);
CombatProcedureSwitchWeapon::ctor(proc, targetWeapon);
CombatController::AddBackgroundProcedure(combatCtrl, proc);  // 0x980270
```
**Result:** Procedure created and added, but weapon did NOT change. The existing
action procedure (attack/reload) was still active and overrode the background switch.
The game uses this internally from the combat planner where the entire plan is rebuilt.
From a ReloadAlt hook it does nothing.

### 6. ExtraAmmo Clip State Check (Dead End for NPCs)
Tried checking per-weapon clip state via ExtraAmmo (type 0x6E) on inventory entries.
**Result:** NPC weapons in inventory do NOT have ExtraAmmo. clipState is always -1.
NPC clip state is tracked entirely through `MiddleHighProcess::pCurrentAmmo` (a
ContChangesEntry whose countDelta = clip rounds). This is ONLY for the currently
equipped weapon. Non-equipped weapons have no clip state at all.

### 7. "Only Switch When Out of Ammo" Approach
Added HasAmmoForWeapon check: only switch if NPC has no reserve ammo for current weapon.
**Result:** NPCs have infinite ammo - they RELOAD, they don't consume reserve ammo.
HasAmmoForWeapon always returns true. Feature never triggers.

### 8. SetActionProcedure + Deferred Clip Fill (Internal Change Only)
```cpp
void* proc = GameHeapAlloc(0x18);
CombatProcedureSwitchWeapon::ctor(proc, targetWeapon);
CombatController::SetActionProcedure(combatCtrl, proc);
// then after switch completes:
Actor::Reload(actor, targetWeapon, 0, false);  // a3=0 = no animation
```
**Result:** GetEquippedWeapon DOES return the new weapon. Clip fill succeeds (result=1).
Log shows weapon alternation A->B->A->B working perfectly.
**BUT:** User reports NPCs don't visually switch. The weapon changes internally
(confirmed by log) but no holster/draw animation plays. The NPC appears to keep
firing the same weapon.

Additional mechanisms tried with this approach:
- **Reload suppression**: When ReloadAlt fires and actor has a PendingFill, return
  without calling Original. Prevents reload animation from starting during switch.
- **ClearRecentSwitch on fill**: When PendingFill completes, clear ping-pong record
  so A->B->A->B alternation is allowed.
- **Increased timeout**: Switch procedure takes 60-300 frames. Original 120 frame
  timeout caused many PendingFills to expire before the switch completed. Increased
  to 600 frames - no more timeouts.

The full internal flow works:
1. A empty -> ReloadAlt fires -> queue switch to B -> add PendingFill(B)
2. B empty -> ReloadAlt fires -> HasPendingFill -> suppress reload (return)
3. ProcessPendingFills: GetEquippedWeapon == B -> Reload(B, a3=0) -> clip filled
4. ClearRecentSwitch -> next cycle can go B->A
5. B empty -> switch to A -> repeat

All confirmed by log. Zero timeouts, zero ping-pong blocks, clip fills all succeed.
**But no visual weapon change.**

## WHY EquipItem Fails (from all contexts)

From decompiled `Actor::EquipItem` (0x88C650):
```cpp
// For NPCs with process level <= 1, it QUEUES the equip:
this->pCurrentProcess->AppendQueuedEquipItem(...);
```

The equip is **queued**, not immediate. The combat AI overrides our equip:
1. Our equip gets queued
2. Combat AI runs weapon selection
3. Combat AI re-equips the original weapon
4. Our queued equip gets overridden

## Why Game's CombatProcedureSwitchWeapon Works (In Theory)

The game's own weapon switch procedure works because:
1. It's a CombatProcedure registered with the CombatController
2. The combat AI knows a switch is in progress
3. The combat AI doesn't interfere with its own procedures

## Actor::ReloadAlt (0x8A83C0) - Trivial Wrapper

Decompiled:
```cpp
void Actor::ReloadAlt(this, TESObjectWEAP* apWeapon, UInt32 a3, bool a4, int a5)
{
    if (TESObjectWEAP::IsReloadLoop(apWeapon))
        this->Reload(this, apWeapon, a3, a4);
    else
        this->Reload(this, apWeapon, a3, a4);
}
```
Both branches call Actor::Reload. The only difference would be in code the
decompiler collapsed. But functionally it's just a Reload wrapper.
Called from DecreaseClipAmmo when clip reaches 0.

## KEY DISCOVERY: NPC Clip State NOT in ExtraAmmo

NPC weapons in inventory do NOT have ExtraAmmo. clipState is always -1 for NPCs.
NPC clip state is tracked entirely through `MiddleHighProcess::pCurrentAmmo` (a
ContChangesEntry whose countDelta = clip rounds). This is ONLY for the currently
equipped weapon.

When a weapon is equipped via CombatProcedureSwitchWeapon, its clip starts at 0.
The NPC must reload. Actor::Reload(a3=0) fills clip without animation.

## Key Addresses (PC 1.4.0.525)

### Actor Functions
- `Actor::ReloadAlt` - 0x8A83C0 (hook point, calls Reload)
- `Actor::Reload` - 0x8A8420 (a3=0 fills clip without animation)
- `Actor::GetEquippedWeapon` - 0x8A1710
- `Actor::GetCombatController` - 0x8A02D0 (returns CombatController*, PDB confirmed)
- `Actor::GetAnimAction` - 0x8A7570 (returns -1 when idle)
- `Actor::GetIsWeaponOut` - 0x8A16D0
- `Actor::SetWantWeaponDrawn` - 0x8A6840
- `Actor::IsInCombat` - vtable index 0x10A

### Combat System
- `CombatProcedureSwitchWeapon::ctor` - 0x9DA720 (size 0x18)
- `CombatProcedureSwitchWeapon::Update` - 0x9DA7C0
- `CombatProcedureSwitchWeapon` vtable - 0x010920CC
- `CombatController::SetActionProcedure` - 0x980110
- `CombatController::SetMovementProcedure` - 0x9801B0
- `CombatController::AddBackgroundProcedure` - 0x980270
- `CombatProcedure::CombatProcedure` - 0x9CA680 (base constructor)
- `CombatProcedure::CreateType` - 0x997A60 (factory, type 5 = SwitchWeapon)
- `CombatActionSwitchWeapon::Execute` - 0x97C790 (COMDAT folded with DrawWeapon)
- `CombatController::HasSwitchWeaponProcedure` - 0x981970

### Memory
- `GameHeapAlloc` - 0xAA3E40 (heap at 0x11F6238)

### ExtraAmmo (works for player, NOT for NPCs)
- `ExtraDataList::GetAmmoExtra` - 0x42EB40
- `ExtraDataList::SetAmmoCount` - 0x42EB60
- `ExtraAmmo::ExtraAmmo` ctor - 0x42EC30
- `TESObjectWEAP::GetClipRounds` - 0x4FE160
- `kExtraData_Ammo` - 0x6E

## Current Status: STUCK

SetActionProcedure changes the weapon internally (GetEquippedWeapon confirms it)
and Actor::Reload(a3=0) fills the clip. The full cycle works in the log.
But NPCs don't visually switch weapons. The holster/equip/draw animation from
CombatProcedureSwitchWeapon either doesn't play or is being bypassed.

Possible causes:
1. Combat AI overrides the action procedure before animations play
2. Our reload suppression (returning early from ReloadAlt) interferes with the
   animation state machine
3. The switch procedure completes internally without visual animation
4. Something about calling SetActionProcedure from a ReloadAlt hook context
   prevents proper animation scheduling

## What Still Needs Investigation
1. Why does the weapon change internally but not visually?
2. Is CombatProcedureSwitchWeapon::Update actually running its holster/equip/draw
   sequence, or is something short-circuiting it?
3. Would hooking at a different point (not ReloadAlt) allow the switch procedure
   to execute properly?
4. Is there a way to force the visual weapon model to update after an internal
   weapon change?
