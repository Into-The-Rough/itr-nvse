# itr-nvse Hook Registry

Registry of the runtime memory patches and hook sites in the current tree.

## Legend
- **Type**: `call` = `WriteRelCall`, `jump` = `WriteRelJump`, `patch` = direct code/data write, `vtable patch` = direct function pointer swap
- **Size**: bytes overwritten at the patch site
- **Chain**: whether the hook reaches original code

## Fixes

### AshPileNames

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x776F66 | call | 5 | continues | yes | Hook_GetBaseFullName |

Call-site replacement in `SetHUDCrosshairStrings`, not a global `GetBaseFullName` detour.

### ArmorDTDRFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8D4E80 | jump | 7 | trampoline | yes | Hook_ResetArmorRating |

### CombatItemTimerFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9DAB61 | call | 5 | continues | yes | Hook_ResetCombatItemTimer |

### CompanionNoInfamy

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8C0E6E | call | 5 | normal | conditional | MurderAlarmReputationHook_Wrapper |
| 0x8C0930 | call | 5 | normal | conditional | AttackAlarmReputationHook_Wrapper |
| 0x89F3DF | call | 5 | normal | conditional | ActorKillReputationHook_Wrapper |

Wrappers load extra context into `EDX` and tail-jump to typed `__fastcall` replacements so the compiler owns cleanup.

### CompanionWeightlessOverencumberedFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x75DE01 | call | 5 | normal | typed replacement | Hook_GetMaxCarryWeightPerkModified_Wrapper |

### DetectionFollowerCrashFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9736AF | jump | 7 | 0x9736B6 / 0x9736F7 | no | Hook |

### DoorPackageOwnershipFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x90D528 | call | 5 | continues | yes | IsAnOwner_Hook |
| 0x90D5DE | call | 5 | continues | yes | IsAnOwner_Hook |

### ExplodingPantsFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9C3204 | jump | 5 | 0x9C3209 | yes | Hook_IsAltTrigger_Wrapper |

### FriendlyFire

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9C314E | patch | 5 | continues | n/a | Projectile hostile-target check bypass |
| 0x899D5A | patch | 1 | continues | n/a | Actor combat-target branch bypass |

### InitHavokCrashFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x576AB3 | jump | 6 | 0x576AB9 / 0x576AD5 | no | Hook |

### KillActorXPFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x5BE379 | jump | 8 | 0x5BE381 / 0x5BE3FA | conditional | Hook_XPBlockStart |

### NavMeshInfoCrashFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x68F320 | jump | 5 | retn | no | Hook |

### NPCDoorUnlockBlock

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x518F00 | jump | 5 | trampoline | yes | CanActorIgnoreLock_Hook |

### NoDoorFade

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x51895B | call | 5 | continues | yes | Hook_FadeOut |

### OwnedBeds

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x509679 | call | 5 | continues | yes | IsAnOwnerHook |

### OwnedCorpses

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x567790 | jump | 6 | trampoline | yes | GetOwnerRawFormHook |
| 0x8BFBB3 | jump | 11 | 0x8BFC52 / 0x8BFBBE | conditional | StealAlarmWitnessHook |

### PathingNullActorFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9E57C6 | jump | 5 | 0x9E57CB / 0x9E5A49 | no | Hook |

### ReversePickpocketNoKarmaFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x75DBDA | call | 5 | continues | conditional | Hook_TryPickpocket |
| 0x75DFA7 | call | 5 | continues | conditional | Hook_TryPickpocket |

### SlowMotionPhysicsFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0xC6AFC5 | call | 5 | continues | yes | Hook_SetFrameTimeMarker |
| 0xC6AFF9 | call | 5 | continues | yes | Hook_StepDeltaTime |

### VATSLimbFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x5E4810 | jump | 6 | trampoline | yes | SetPartitionVisible_Hook |

### VATSProjectileFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x7ED349 | patch | 4 | continues | yes | VATSMenuUpdate_Hook |

Only the 4-byte call displacement is rewritten; the `E8` opcode stays in place.

### VATSSpeechFix

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x10A3C18 | vtable patch | 4 | n/a | yes | HookedFunc09 |
| 0x10A3C1C | vtable patch | 4 | n/a | yes | HookedFunc10 |
| 0xAEDFBD | jump | 14 | trampoline | conditional | HookedTimescaleNaked |

The inline detour at `0xAEDFBD` is only installed when the site still matches the known vanilla bytes.

## Features

### AutoQuickLoad

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x86E88C | call | 5 | continues | yes | PollControlsHook |

### ELMO

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x5EC653 | call | 5 | continues | yes | ShowAsCornerMessage |
| 0x5EC6BA | call | 5 | continues | yes | ShowAsCornerMessage |
| 0x77377E | call | 5 | continues | yes | ShowAsCornerMessage |
| 0x6155F0 | jump | 5 | retn | no | ReputationPopup_Hook |
| 0x615F4A | jump | 5 | 0x615F71 | no | ReputationCornerMessage_Hook_AddRep |
| 0x61598B | jump | 5 | 0x6159B2 | no | ReputationCornerMessage_Hook_AddRepExact |
| 0x615C43 | jump | 5 | 0x615C6A | no | ReputationCornerMessage_Hook_RemRepExact |
| 0x616242 | jump | 5 | 0x616269 | no | ReputationCornerMessage_Hook_RemRep |

### LocationVisitPopup

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x7795DD | jump | 7 | 0x7795E4 | no | CheckDiscoveredMarkerHook |

### MessageBoxQuickClose

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x107566C + 0x38 | vtable patch | 4 | n/a | yes | MessageMenu_HandleSpecialKeyInput_Hook |
| 0x107566C + 0x0C | vtable patch | 4 | n/a | yes | MessageMenu_HandleClick_Hook |

When `QuickReadNote` is also enabled, it chains after `MessageBoxQuickClose` through a post-click observer instead of patching the same slot again.

### PlayerUpdateHook

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x940C78 | call | 5 | continues | yes | PlayerUpdate_Hook |

### PreventWeaponSwitch

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x9DA7C0 | jump | 6 | trampoline | yes | Hook_SwitchWeaponUpdate |

### QuickReadNote

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x966B0A | call | 5 | continues | yes | OnNoteAddedHook |
| 0x966B53 | call | 5 | continues | yes | OnQueueUIMessageHook |
| 0x107566C + 0x0C | vtable patch | 4 | n/a | conditional | MessageMenu_HandleClick_Hook |

If `MessageBoxQuickClose` owns `MessageMenu::HandleClick`, `QuickReadNote` registers a post-click observer instead of taking the vtable slot.

### VATSExtender

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x800DA4 | jump | 5 | varies | yes | Hook_OnLimitReached |
| 0x801993 | call | 5 | continues | yes | Hook_RenderScene |

## Commands

### ToggleAllPrimitives

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x56B4DC | call | 5 | continues | yes | Load3D_PrimitiveCullHook |
| 0x56BAC8 | call | 5 | continues | yes | Load3D_DoorTravelCullHook |

Both are call-site replacements inside `TESObjectREFR::Load3D` so newly loaded primitive refs, door markers, and travel markers honor the current visibility state instead of being force-culled.

## Handlers

### CornerMessageHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x775380 | jump | 5 | trampoline | yes | Hook_HUDMainMenu_ShowNotify |

### DialogueCameraHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x953124 | call | 5 | continues | conditional | Hook_Show1stPerson |
| 0x761DEF | call | 5 | continues | conditional | Hook_Show1stPerson |
| 0x953ABF | jump | 8 | 0x953AF5 | no | Hook_ForceThirdPerson_Branch1 |
| 0x762E55 | jump | 7 | 0x762E72 | no | Hook_ForceThirdPerson_Branch2 |
| 0x9533BE | jump | 6 | 0x953562 | no | Hook_DisableDialogueZoom |
| 0x953BB4 | jump | 8 | branch | no | Hook_SkipFallbackFOV |
| 0x953B2F | call | 5 | continues | conditional | Hook_SkipPickAnimations |
| 0x953AC7 | call | 5 | continues | conditional | Hook_SkipSetFirstPerson |
| 0x94AD8A | call | 5 | continues | yes | TranslateHook1 |
| 0x94AD9D | call | 5 | continues | yes | RotateHook1 |
| 0x94BDC2 | call | 5 | continues | yes | TranslateHook2 |
| 0x94BDD5 | call | 5 | continues | yes | RotateHook2 |

The first eight sites control dialogue flow; the last four are the `CameraHooks::InstallHooks()` call-site replacements for flycam and update-camera transforms.

### DialogueTextFilter

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x61F170 | jump | 6 | 0x61F176 / chained | conditional | DialogueTextHook |
| 0x8A20D0 | jump | 5 | 0x8A20D5 / chained | conditional | SpeakSoundHook |

### FallDamageHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8A63EC | jump | 9 | 0x8A63F5 | no | Hook |

### OnCombatProcedureHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x980110 | jump | 6 | trampoline | yes | Hook_SetActionProcedure |
| 0x9801B0 | jump | 6 | trampoline | yes | Hook_SetMovementProcedure |

### OnContactHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x623CB0 | jump | 6 | trampoline | yes | Hook_ContactPointAdded |
| 0xCAD480 | jump | 10 | trampoline | yes | Hook_CharProxyContactAdded |
| 0xCAD4C0 | jump | 10 | trampoline | yes | Hook_CharProxyContactRemoved |
| 0xD1F690 | jump | 5 | trampoline | yes | Hook_SimpleAdd |
| 0xD1F5A0 | jump | 8 | trampoline | yes | Hook_SimpleRemove |
| 0xD4CF10 | jump | 6 | trampoline | yes | Hook_CachingAdd |
| 0xD4CCA0 | jump | 7 | trampoline | yes | Hook_CachingRemove |

### OnEntryPointHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x5E5AB9 | jump | 5 | 0x5E5ABE | yes | Hook_ExecuteFunctionCall |

### OnFrenzyHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8B9240 | jump | 9 | trampoline | yes | Hook_LimbCondition_HandleChange |

### OnJumpLandHandler

No inline or vtable hooks. This handler polls from `kMessage_MainGameLoop`.

### OnSoundPlayedHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0xAE5A50 | jump | 5 | trampoline | yes | HookedGetSoundHandle |

### OnStealHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8BFA40 | jump | 5 | 0x8BFA45 | yes | StealAlarmHook |

### OnWeaponDropHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x89F580 | jump | 6 | 0x89F586 | yes | TryDropWeaponHook |

### OnWeaponJamHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x894081 | call | 5 | continues | yes | Hook_SetAnimAction_Jam |

### SaveFileSizeHandler

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x7D6806 | patch | 6 | continues | n/a | save/load JNZ skip removal |
| 0x7D6931 | call | 5 | continues | yes | Hook |

## Commands

### DisableCrouching

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x8B39F0 | jump | 7 | trampoline | yes | Hook_SetMovementFlag |
| 0x981520 | jump | 7 | trampoline | yes | Hook_SetShouldSneak |

Custom trampolines on `Actor::SetMovementFlag` and `CombatController::SetShouldSneak` keep disabled actors standing across all callers.

### ForceCombatTarget

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x986C60 | jump | 10 | trampoline | yes | Hook_EvalueCombatTargets |
| 0x8B0670 | jump | 6 | trampoline | yes | Hook_CanAttackActor |

### NoWeaponSearch

| Hook Site | Type | Size | Return | Chain | Function |
|-----------|------|------|--------|-------|----------|
| 0x998D50 | call | 5 | continues | yes | Hook |

## Safety Checklist

When adding new hooks:
1. Verify instruction boundaries in IDA before choosing a stolen-byte size.
2. Document every extra NOP or byte patch, not just the initial `E8`/`E9`.
3. If a wrapper tail-jumps, make sure the hook site is a jump path too.
4. If a hook can run off the main thread, document how state access is synchronized.
5. Keep call-site/vtable ownership notes current when multiple features layer on the same menu or engine path.
