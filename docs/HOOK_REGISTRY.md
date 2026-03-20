# itr-nvse Hook Registry

Central documentation of all memory patches and hooks in the plugin.

## Legend
- **Type**: `call` = WriteRelCall, `jump` = WriteRelJump, `patch` = direct write
- **Size**: bytes overwritten at hook site
- **Chain**: whether hook calls original function

---

## Fixes

### CompanionNoInfamy
Prevents infamy when companion kills faction members.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8C0E6E | call | 5 | ret 8 | conditional | MurderAlarmReputationHook |
| 0x8C0930 | call | 5 | ret 8 | conditional | AttackAlarmReputationHook |
| 0x89F3DF | call | 5 | ret 8 | conditional | ActorKillReputationHook |

Stack: standard call site, args on stack. Hooks check teammate status and skip reputation call if true.
Verified 2026-03-20: `Actor::HandleMajorCrimeFactionReputations(0x8B7D20)` and `Actor::HandleMinorCrimeFactionReputations(0x8B7C00)` both end in `retn 8`.

### ExplodingPantsFix
Fixes explosive pants bug with alt trigger weapons.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9C3204 | jump | 5 | 0x9C3209 | yes | Hook_IsAltTrigger_Wrapper |

Stack: ECX=weapon, hook preserves and chains.

### KillActorXPFix
Fixes XP calculation on actor kill.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8BC43F | jump | 5 | 0x8BC446 | yes | Hook_XPBlockStart |

Stack: preserves all, uses pushad/popad.

### OwnedBeds
Allows sleeping in owned beds when no owner present.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x509679 | call | 5 | continues | yes | IsAnOwnerHook |

Stack: __fastcall, ECX=owner, EDX=cell.

### ReversePickpocketNoKarmaFix
Prevents karma loss when reverse pickpocketing non-grenades.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x75DBDA | call | 5 | continues | conditional | Hook_TryPickpocket |
| 0x75DFA7 | call | 5 | continues | conditional | Hook_TryPickpocket |

Stack: `HandlePickpocket(0x75E0B0)` is `__thiscall` and ends in `retn 8`. The hook is now a typed `__fastcall` replacement with `ECX=menu` and stack args `actor`, `count`, so the compiler owns cleanup.

### CompanionWeightlessOverencumberedFix
Allows giving zero-weight items to overencumbered companions.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x75DE17 | jump | 5 | 0x75DE1C/0x75DEA5 | conditional | Hook_OverburdenedBranch |

Stack: uses EBP frame locals in `ContainerMenu::TransferItem` to compute added transfer weight (`[ebp-0x14C] - [ebp-0x144]`).

### SlowMotionPhysicsFix
Prevents physics explosion during extreme slowmo.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0xC6AF85 | patch | 4 | continues | yes | Hook_SetFrameTimeMarker |
| 0xC6AFF9 | patch | 4 | continues | yes | Hook_StepDeltaTime |

Stack: __thiscall, ECX=bhkWorld. Clamps timestep to minimum.

### VATSLimbFix
Hides dismembered limbs in VATS targeting.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x5E4810 | jump | 6 | trampoline | yes | SetPartitionVisible_Hook |

Trampoline: 6 bytes (push ebp; mov ebp,esp; sub esp,8). Uses __fastcall.

### VATSProjectileFix
Fixes VATS projectile display issues.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x7ED349 | patch | 4 | continues | yes | VATSMenuUpdate_Hook |

Stack: __thiscall, chains to original VATSMenu::Update.

### VATSSpeechFix
Fixes timescale during VATS speech.

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| (see file) | - | - | - | - | HookedTimescaleNaked |

---

## Features

### ELMO (objectives/reputation to corner messages)

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x77A5B0 | jump | 5 | ret | no | Hook_SetQuestUpdateText |
| 0x6155F0 | jump | 5 | retn | no | ReputationPopup_Hook |
| 0x615F4A | jump | 5 | 0x615F71 | no | ReputationCornerMessage_Hook_AddRep |
| 0x61598B | jump | 5 | 0x6159B2 | no | ReputationCornerMessage_Hook_AddRepExact |
| 0x615C43 | jump | 5 | 0x615C6A | no | ReputationCornerMessage_Hook_RemRepExact |
| 0x616242 | jump | 5 | 0x616269 | no | ReputationCornerMessage_Hook_RemRep |

Stack: pushad/popfd pattern, calls FormatReputationMessage and QueueUIMsg.

### LocationVisitPopup

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x7795DD | jump | 5 | 0x7795E4 | no | CheckDiscoveredMarkerHook |

Stack: ESI=marker data, EBX=refID. Uses pushad/popad.

### PlayerUpdateHook (QuickDrop/Quick180)

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x940C78 | patch | 4 | continues | yes | PlayerUpdate_Hook |

Stack: __fastcall replacement for player update. ECX=player, arg1=timeDelta.

### QuickReadNote

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x966B0A | call | 5 | continues | yes | OnNoteAddedHook |
| 0x966B53 | call | 5 | continues | yes | OnQueueUIMessageHook |

Stack: naked hooks, save context with pushad.

### VATSExtender

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x800DA4 | jump | 5 | varies | yes | Hook_OnLimitReached |
| 0x801993 | call | 5 | continues | yes | Hook_RenderScene |

---

## Handlers

### DialogueCameraHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x953124 | nop | 5 | - | - | (disabled code) |
| 0x761DEF | nop | 5 | - | - | (disabled code) |
| 0x953ABF | jump | 5 | 0x953AF5 | no | (skip code) |
| 0x762E55 | jump | 5 | 0x762E72 | no | (skip code) |
| 0x9533BE | jump | 5 | 0x953562 | no | (skip zoom) |
| 0x953BBA | patch | 1 | - | - | 0xEB (jmp) |

Plus camera hooks installed via CameraHooks::InstallHooks().

### FallDamageHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8A63EC | jump | 5 | 0x8A63F5 | no | Hook |

Stack: [ebp-0x54]=actor, [ebp-0x28]=damage. Applies multiplier via GetFallDamageMultForActor.

### OnJumpLandHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x10CB398 + (8*4) | vtable patch | 4 | n/a | yes | Hook_bhkCharacterStateJumping_UpdateVelocity |
| 0x10CB36C + (8*4) | vtable patch | 4 | n/a | yes | Hook_bhkCharacterStateInAir_UpdateVelocity |

Captures jump start and landing transitions. Landing queues pre-clear `fallTimeElapsed`.

### OnCombatProcedureHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| (see file) | - | - | - | - | Hook_SetActionProcedure |
| (see file) | - | - | - | - | Hook_SetMovementProcedure |

### OnEntryPointHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x5E5AB9 | jump | 5 | 0x5E5ABE | yes | Hook_ExecuteFunctionCall |

Stack: inline detour over the original `call BGSEntryPointFunction::ExecuteFunction`. The hook calls `0x5E5B40` itself, then jumps to `0x5E5ABE` so the caller's `add esp, 18h` still runs.

### OnFastTravelHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x93BF22 | call | 5 | continues | yes | FastTravelHook |

Stack: naked hook, chains to original via saved target.

### OnStealHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8BFA40 | jump | 5 | 0x8BFA45 | yes | StealAlarmHook |

Stack: ECX=thief, [esp+4..14]=args. Prologue: push ebp; mov ebp,esp; push -1 (5 bytes).

### OnWeaponDropHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| (see file) | - | - | - | - | TryDropWeaponHook |

### OnWeaponJamHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x894081 | call | 5 | continues | yes | Hook_SetAnimAction_Jam |

Stack: ECX=actor, args follow. Dispatches jam event, then tail-jumps the original callee `Actor::SetAnimAction(0x8A73E0)`.

### SaveFileSizeHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x7D6806 | nop | 6 | - | - | (JNZ patch) |
| 0x7D6931 | call | 5 | continues | yes | Hook |

Stack: ECX=tile, [ebp+C]=entry. Calls `OnSetupTile`, then tail-jumps the original callee `Tile::PropagateIntValue`.

---

## NoWeaponSearch (ITR.cpp)

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x998D50 | call | 5 | continues | yes | Hook |

Disables weapon search for specific actors.

---

## Safety Checklist

When adding new hooks:
1. Verify instruction boundary (disassemble target)
2. Document bytes overwritten
3. If using trampoline, copy exact prologue
4. If using pushad/popad, ensure stack alignment
5. If chaining, verify calling convention matches
6. Add entry to this registry
