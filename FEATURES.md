# itr-nvse

## Commands

**String**
- Sv_TrimStr - trim whitespace from string
- Sv_Join - join array elements with delimiter
- Sv_Reverse - reverse a string

**Utility**
- GetRefsSortedByDistance - refs sorted by distance with filters
- Duplicate - duplicate a form
- GetAvailableRecipes - get available crafting recipes
- ModChallenge - modify challenge progress
- DamageActorValueAlt - extended actor value damage
- ToggleAllPrimitives (TAP) - toggle primitive refs plus regular marker refs with fixed load-time behavior

**Actor**
- ResurrectActorEx - resurrect with options
- ResurrectAll - resurrect all dead actors in area
- SetCreatureCombatSkill - set creature combat skill
- SetRaceAlt - set actor race at runtime
- UseAidItem - force actor to use an aid item
- ForceCrouch - force actor into crouch
- DisableCrouching - prevent actor from crouching
- ForceCombatTarget - force actor combat target
- ForceSay - force actor dialogue speech
- IsSaying - check if actor is speaking

**Fall Damage**
- SetFallDamageMult - set fall damage multiplier per actor
- GetFallDamageMult - get fall damage multiplier
- ClearFallDamageMult - clear fall damage multiplier

**Combat**
- FakeHit - simulate hit on actor
- FakeHitEx - simulate hit with extended params
- SetNoWeaponSearch - disable weapon search for actor
- GetNoWeaponSearch - get weapon search state
- SetPreventWeaponSwitch - prevent NPC weapon switching
- GetPreventWeaponSwitch - get weapon switch prevention state
- ForceReload - force weapon reload
- RefillAmmo - add ammo to actor inventory and fill clip
- Gesture - procedural head gestures (nod/shake/tilt) with smoothstep blending
- SetOnContactWatch - enable physics contact tracking for actor
- GetOnContactWatch - get contact watch state
- GetTargetLastSeenLocation - array [x,y,z] of where observer last saw target
- GetTargetDetectedLocation - array [x,y,z] of where observer detected target by sound/event
- GetTargetLastFullyVisibleLocation - array [x,y,z] of where observer last had full LOS to target
- GetTargetInitialLocation - array [x,y,z] of where observer first spotted target

**Camera**
- SetCameraAngle - direct camera transform control
- SetDialogueCameraEnabled - runtime toggle for dialogue camera
- SetDialogueCameraMode - auto angle mode: cycle, fixed, random, manual
- SetDialogueCameraFixedAngle - fixed angle used by fixed mode
- SetDialogueCameraAngle - immediate exact or random angle switch
- SetDialogueCameraDolly - dialogue camera dolly effect
- SetDialogueCameraShake - dialogue camera shake override
- Modes: 0=cycle, 1=fixed, 2=random, 3=manual
- Angle IDs: 0=Vanilla, 1=OverShoulder, 2=NPCCloseup, 3=TwoShot, 4=NPCFace, 5=LowAngle, 6=HighAngle, 7=PlayerFace, 8=WideShot, 9=NPCProfile, 10=PlayerProfile, 11=Overhead

**Weapon Visuals**
- SetWeaponEmissiveColor - set weapon emissive color
- ClearWeaponEmissiveColor - clear weapon emissive color

**Radio**
- IsRadioPlaying - returns 1 if pip-boy or ambient radio is currently playing
- GetPlayingRadioTrack - returns TESSound or TESTopicInfo for playing radio track
- GetPlayingRadioTrackFileName - returns file path of playing radio track
- GetPlayingRadioText - returns dialogue text of playing radio voice line
- ChangeRadioTrack - advances active radio station to next track

**Dialogue**
- GetDialogueInfoFlags - get combined flags for a TESTopicInfo
- SetDialogueInfoFlags - set combined flags for a TESTopicInfo (runtime)
- GetDisplayedDialogueInfos - get array of topic infos shown in dialogue menu

**UI**
- SetUIAlphaMap - applies an alpha-map texture to a UI image tile
- SetUITexOffset - scrolls a UI image tile's texture coordinates

**Input**
- DisableKeyEx - disable key with handler
- EnableKeyEx - enable key with handler

**Ground/Terrain**
- MoveToTerrain - move reference to terrain height
- GetDistanceToTerrain - get distance to terrain
- MoveToGround - move reference to ground
- GetDistanceToGround - get distance to ground

## Event Handlers

- ITR:OnSteal - fires when items stolen (thief, target, item, owner, quantity)
- ITR:OnWeaponJam - fires when weapon jams (actor, weapon)
- ITR:OnWeaponDrop - fires when actor attempts weapon drop (actor, weapon)
- ITR:OnFrenzy - fires when actor enters frenzy (actor)
- ITR:OnCornerMessage - fires on HUD corner message (text, emotion, icon, sound, time, metaType)
- ITR:OnDialogueText - fires on dialogue text display (speaker, topic, response, text, responseText)
- ITR:OnDoubleTap - fires on double-tap key press (keyCode)
- ITR:OnKeyHeld - fires while key held past threshold (keyCode, heldSeconds)
- ITR:OnCombatProcedure - fires on combat AI procedure change (actor, procType, isAction)
- ITR:OnEntryPoint - fires when perk entry points execute (perk, entryPoint, actor, filterForm)
- ITR:OnActorLanded - fires when actor lands (actor, fallTime)
- ITR:OnJumpStart - fires when actor starts jumping (actor)
- ITR:OnConsoleOpen - fires on console open
- ITR:OnConsoleClose - fires on console close
- ITR:OnKeyDisabled - fires when key disabled (keyCode, controlID)
- ITR:OnKeyEnabled - fires when key enabled (keyCode, controlID)
- ITR:OnMenuFilterChange - fires on menu filter tab change (menuID, oldFilter, newFilter, filterCount)
- ITR:OnMenuSideChange - fires on menu side change (menuID, oldSide, newSide)
- ITR:OnSoundPlayed - fires on sound playback (filePath, flags, sourceForm)
- ITR:OnSoundCompleted - fires when tracked voice sound completes (filePath, flags, sourceForm)
- ITR:OnContactBegin - fires when physics contact begins on watched actor (actor, contactType)
- ITR:OnContactEnd - fires when physics contact ends on watched actor (actor, contactType)
- ITR:OnWitnessed - fires per witness per crime (witness, perpetrator, crimeType, victim, detectionValue). Filterable on witness/perpetrator/victim refs/formlists/factions and on crimeType equality.
- ITR:OnImpactDataSpawn - fires when a projectile resolves ImpactData on a non-actor hit (impactData, x, y, z, normalX, normalY, normalZ, projectile, target, weapon, material)
- ITR:OnSprayDecal - fires per blood spray decal placement during limb sever/explode (impactData, x, y, z, normalX, normalY, normalZ)

## Features

- QuickDrop - drop weapon with hotkey combo
- Quick180 - rotate player 180 degrees instantly
- QuickReadNote - view notes on pickup without pip-boy, play holotapes inline
- LocationVisitPopup - notification popup when revisiting discovered locations
- VATSExtender - more VATS targets beyond vanilla limit
- CameraOverride - direct camera transform control via script
- DialogueCamera - script-driven camera angles during dialogue with cycling, shake, dolly
- PreventWeaponSwitch - stop NPCs switching weapons mid-combat (per-actor)
- NoWeaponSearch - disable weapon search for specific actors (per-actor)
- NPCAntidoteUse - NPCs use antidotes when poisoned in combat (configurable cooldown)
- NPCDoctorsBagUse - NPCs use doctor's bags when crippled in combat (configurable cooldown)
- ELMO - convert quest objectives and reputation popups to corner messages
- MessageBoxQuickClose - quick close message boxes with Enter/Space
- OwnerNameInfo - show item owner on crosshair prompt
- SaveFileSize - show save file size in save/load menu
- AutoQuickLoad - auto-load quicksave on main menu
- AutoGodMode - god mode on game start
- AltTabMute - mute audio when alt-tabbed
- PlayerUpdateHook - host for QuickDrop and Quick180 input detection

## Fixes

- SlowMotionPhysicsFix - clamp physics timestep to prevent ragdoll energy gain during extreme slowmo
- ExplodingPantsFix - prevent explosions from worn projectile items
- VATSProjectileFix - fix projectile hit chance in VATS by correcting visibility
- VATSLimbFix - hide dismembered limbs from VATS targeting
- VATSSpeechFix - prevent voice/dialogue from slowing during VATS timescale
- KillActorXPFix - prevent XP from kill command on already-dead actors
- ArmorDTDRFix - force NPC armor DT/DR cache update on equipment change
- CombatItemTimerFix - fix stimpak timer using wrong game setting
- FriendlyFire - enable player and ally friendly fire damage
- OwnedBeds - allow sleeping in owned beds with assault alarm consequences
- OwnedCorpses - looting owned corpses counts as stealing
- NoDoorFade - skip actor fade animation on door entry
- AshPileNames - show original NPC name for ash piles
- ReversePickpocketNoKarmaFix - no karma loss on reverse pickpocket of non-explosives
- CompanionNoInfamy - companion kills don't give player faction infamy
- DoorPackageOwnershipFix - fix NPCs with lock/unlock packages locking doors in cells they don't own
- NPCDoorUnlockBlock - configurable NPC door unlock restrictions (vanilla/strict/full)
- MusicResetOnLoad - reset music state when loading a save to fix stuck or missing music
- PathingNullActorFix - fix crash from null actor in pathing code
- NavMeshInfoCrashFix - fix crash in NavMesh info processing
- InitHavokCrashFix - fix crash during Havok physics initialization
- DetectionFollowerCrashFix - fix null dereference in BuildFollowerListRecursive during cell transitions
- ConsoleLogCleaner - delete console log on startup

## Configuration

INI at `Data/config/itr-nvse.ini`, reload with `ReloadPluginConfig itr-nvse` console command.

MCM support via MCM Extender.

## Integration

Other mods can opt into itr-nvse features by placing INI files in `Data/config/itr/`.

Supported keys:

| Section | Key | Effect |
|---------|-----|--------|
| CornerMessage | bSuppressSound | Strip sound from vanilla ShowNotify, let event handler play it at display time |
