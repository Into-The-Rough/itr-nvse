# itr-nvse

## Compatibility notes

- 2.2.0: ITR:OnPreHitDamage moved from hit-calculation finalizers to Actor::HitMe. It now fires only for applied actor hits and reports final post-dmgMult damage. Hits caused synchronously by an event handler are applied without recursively dispatching; hits raised by vanilla OnHit processing after the callback returns dispatch normally

## Commands

**String**
- Sv_TrimStr - trim whitespace from string
- Sv_Join - join array elements with delimiter
- Sv_Reverse - reverse a string

**Utility**
- GetRefsSortedByDistance - refs sorted by distance with filters, optional heading-cone and result-count cap
- Duplicate - duplicate a form
- GetAvailableRecipes - get available crafting recipes
- SpawnTempParticle - spawn a temporary particle NIF at an explicit world position, aligned to a surface normal. Calling-ref command: path, duration, x, y, z, normalX, normalY, normalZ, scale, optional yawDegrees
- ModChallenge - modify challenge progress
- DamageActorValueAlt - extended actor value damage
- ToggleAllPrimitives (TAP) - toggle primitive refs plus regular marker refs with fixed load-time behaviour
- CenterOnCellAlt (COCA) - dispatch a synthetic new-session message before COC when launched from the main menu
- GetRefExteriorDoor - exterior-side load door reachable from a reference
- GetRefNextTeleportDoor - next load door on the current shortest path to a reference

**Actor**
- ResurrectActorEx - resurrect with options
- ResurrectAll - resurrect all dead actors in area
- SetCreatureCombatSkill - set creature combat skill
- SetRaceAlt - set actor race at runtime. Note: the race swap uses a runtime-created form and does not persist through save/load — re-apply it in a load-game handler (e.g. ITR's own events or a quest script).
- SetEyeMesh - override an eyes form's left/right mesh (run Update3D on an actor to apply)
- ClearEyeMesh - remove an eyes form mesh override (run Update3D on an actor to apply)
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
- FakeImpact - spawn a weapon's impact effect (particle + sound) on the calling object
- SetNoWeaponSearch - disable weapon search for actor
- GetNoWeaponSearch - get weapon search state
- SetPreventWeaponSwitch - prevent NPC weapon switching
- GetPreventWeaponSwitch - get weapon switch prevention state
- ForceReload - force weapon reload
- RefillAmmo - add ammo to actor inventory and fill clip
- Ragdoll - force actor ragdoll with directional knock, limb weighting, spin/tumble and front/back flip
- RagdollLimb - jolt one limb of an already-ragdolling actor with a directional impulse
- SetDismemberedLimbVelocity - set world linear or angular velocity on an engine-created detached limb copy; uses DismemberLimb body-part IDs, or -1 for all, with an optional detached bone name
- Gesture - procedural head gestures (nod/shake/tilt) with smoothstep blending, composed additively on the live animated pose so gestures layer over headtracking
- IsRigidBodyAtRest - check whether loaded mobile Havok rigid bodies under a ref are inactive
- SetOnContactWatch - enable physics contact tracking for a ref, or for all refs using a base form
- GetOnContactWatch - get ref or base form contact watch state
- GetTargetLastSeenLocation - array [x,y,z] of where observer last saw target
- GetTargetDetectedLocation - array [x,y,z] of where observer detected target by sound/event
- GetTargetLastFullyVisibleLocation - array [x,y,z] of where observer last had full LOS to target
- GetTargetInitialLocation - array [x,y,z] of where observer first spotted target

**Camera**
- SetCameraAngle - direct camera transform control
- SetAimZoomFirstPersonOnly - runtime toggle for first-person-only aiming zoom
- GetAimZoomFirstPersonOnly - current first-person-only aiming zoom state
- SetDialogueCameraEnabled - runtime toggle for dialogue camera
- SetDialogueCameraMode - auto angle mode: cycle, fixed, random, manual
- SetDialogueCameraFixedAngle - fixed angle used by fixed mode
- SetDialogueCameraAngle - immediate exact or random angle switch
- SetDialogueCameraDolly - dialogue camera dolly effect
- SetDialogueCameraShake - dialogue camera shake override
- Modes: 0=cycle, 1=fixed, 2=random, 3=manual
- Angle IDs: 0=Vanilla, 1=OverShoulder, 2=NPCCloseup, 3=TwoShot, 4=NPCFace, 5=LowAngle, 6=HighAngle, 7=PlayerFace, 8=WideShot, 9=NPCProfile, 10=PlayerProfile, 11=Overhead

**Weapon Visuals**
- SetWeaponEmissiveColor - set weapon emissive colour
- ClearWeaponEmissiveColor - clear weapon emissive colour

**Radio**
- IsRadioPlaying - returns 1 if pip-boy or ambient radio is currently playing
- GetPlayingRadioTrack - returns TESSound or TESTopicInfo for playing radio track
- GetPlayingRadioTrackFileName - returns file path of playing radio track
- GetPlayingRadioText - returns dialogue text of playing radio voice line
- ChangeRadioTrack - advances active radio station to next track
- PlayRadioFile - skips the current track and plays a wav/ogg/mp3 file (relative to Data\Sound\) on the active station. The engine feeds its two radio outputs from different decoders, so wav/ogg play on the sound-handle output that serves pip-boy voice segments and world speakers, and mp3 on the pip-boy song stream; each output takes the next queued entry of its matching format, so mixed-format queues feed both outputs without blocking each other and mp3 is not guaranteed to be the immediately next segment. Subtitles are unavailable for injected files
- QueueRadioTrack - queues a wav/ogg/mp3 file to play on the next natural track advance of its matching output (queue of 8, returns 0 when full)
- ClearRadioQueue - clears the injected track queue, returns entries removed
- SetRadioQueueLoop - 1 cycles the queue as a playlist instead of consuming it, 0 restores consume
- GetRadioQueueSize - returns the injected queue count

**Dialogue**
- GetDialogueInfoFlags - get combined flags for a TESTopicInfo
- SetDialogueInfoFlags - set combined flags for a TESTopicInfo (runtime)
- GetDisplayedDialogueInfos - get array of topic infos shown in dialogue menu
- AddDialogueTopicEntry - appends a synthetic row to the open dialogue menu that fires ITR:OnDialogueTopicSelected on click instead of running an INFO (prompt, syntheticId up to 24 bits). Call from an ITR:OnDialogueMenuBuild handler
- SetDialogueTopicEntryAlpha - darken/disable the vanilla dialogue row topic_<index> via its _line_alpha trait
- SetDialogueTopicHidden - hide or show a dialogue row by its TESTopic or TESTopicInfo (topicOrInfo, hidden). Applied on the next list rebuild (menu open or after each reply)
- SetDialogueTopicOrder - sort a dialogue row by its TESTopic or TESTopicInfo, lower orders first (topicOrInfo, order). Applied on the next list rebuild
- ClearDialogueTopicOverrides - clear all topic hide and order rules, returns the number removed

**UI**
- SetUIAlphaMap - applies an alpha-map texture to a UI image tile
- SetUITexOffset - scrolls a UI image tile's texture coordinates
- WatchTileValue - watch a tile trait ("menuname/path/to/tile", "traitname") for value changes via ITR:OnTileValueChange, returns a watch id. Watches are runtime-only and drop when the menu closes or a game loads
- UnwatchTileValue - stop watching by watch id

**Navmesh**
- GetPathLength - complete solved-path length from source (default: calling ref, must be an actor) to target, -1 on failure
- IsPointOnNavmesh - returns 1 if x,y,z resolves to a loaded navmesh triangle (optional anchor ref supplies the cell, default player)
- GetCoverPointsInRadius - array of [x, y, z, coverFlags] for cover edges on loaded navmeshes near a point (unsorted, max 128). coverFlags: bits 0-3 cover height bucket (16 units each), bit 4 left open, bit 5 right open, bit 6 edge slot
- GetBestCoverFromThreat - array of [x, y, z, coverFlags, distToSearch] for cover near a search point that hides the standing position from a threat (blocked line of sight), sorted nearest-first, empty when none. Args: threatX threatY threatZ searchX searchY searchZ radius [maxResults=8, cap 32]
- GetActorCoverState - the actor's from-cover procedure state (0 INITIALIZING, 1 WAITING_BEHIND_COVER, 2 MOVING_OUT, 3 WAITING_OUT_OF_COVER, 4 FIRING_OUT_OF_COVER, 5 MOVING_IN, 6 MOVING_IN_AND_ROTATE, 7 HOLDING_GROUND), -1 when no from-cover action is running
- GetActorCoverInfo - string map for the navmesh triangle the actor stands on: triangle, navmesh, hasCover, edges (per cover edge: slot, heightBucket, leftOpen, rightOpen, edgeA, edgeB). Adds state, stateName, hasProcedure and a cover sub-map when the combat AI has cover reserved. 0 only when the actor is off the navmesh with no reserved cover

**Perk**
- GetEligiblePerks - returns array of perks the player can currently select from the level-up perk menu. Flags: 1 hidden, 2 non-playable, 4 traits, 8 owned max rank, 16 non-level-up, 256 requirement-blocked. The delta array argument only compiles in geck scripts, the runtime compiler rejects array params
- GetEligiblePerksEx - GetEligiblePerks for runtime-compiled scripts (flags, optional levelOverride). Same result format, no delta array needed
- GetPerkEligibility - check if a perk is eligible and return detailed eligibility data
- GetPerkBlockers - get requirements blocking a perk
- GetPerksForForm - get perks associated with a form

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
- ITR:OnSoundPlayedByPosition - fires per sound like OnSoundPlayed but adds spatial data (filePath, flags, sourceSound, hasPos, x, y, z, volume). Fires for every sound; hasPos is 0 and volume 1.0 when the engine gave the sound no 3D position before dispatch. Position/volume are the creation-time snapshot, later movement of looping sounds is not tracked.
- ITR:OnActorLanded - fires when actor lands (actor, fallTime)
- ITR:OnJumpStart - fires when actor starts jumping (actor)
- ITR:OnActorFootstep - fires synchronously for actor footstep animation notes (actor, rawSoundID, side, footX, footY, footZ). side is 0 left, 1 right, or -1 unknown. Non-biped skeletons use the actor position when the named foot node is unavailable
- ITR:OnActorFootContact - fires synchronously when a biped footstep resolves ground beneath the named foot (actor, rawSoundID, side, x, y, z, normalX, normalY, normalZ, material, collisionLayer). The event is actor-agnostic; consumers choose which actors to handle. Contact work is skipped when the event has no listeners
- ITR:OnConsoleOpen - fires on console open
- ITR:OnConsoleClose - fires on console close
- ITR:OnConsoleCommand - fires when a typed console command or bat file line reaches Script::Run (sCommand, sFullCommand, rCalleeRef). `sCommand` is lower-case and strips ref prefixes, so `player.additem` dispatches `additem`.
- ITR:OnKeyDisabled - fires when key disabled (keyCode, controlID)
- ITR:OnKeyEnabled - fires when key enabled (keyCode, controlID)
- ITR:OnMenuFilterChange - fires on menu filter tab change (menuID, oldFilter, newFilter, filterCount)
- ITR:OnMenuSideChange - fires on menu side change (menuID, oldSide, newSide)
- ITR:OnMenuListRefresh - fires after a menu's tile list rebuilds for any reason - add/remove/sell/drop/craft/equip (menuID). Inventory (1002), Container (1008), Barter (1053), Recipe (1077).
- ITR:OnSoundPlayed - fires on sound playback (filePath, flags, sourceForm). sourceForm is 0 for sounds played by file path with no backing form
- ITR:OnSoundCompleted - fires when tracked voice sound completes (filePath, flags, sourceForm)
- ITR:OnContactBegin - fires when physics contact begins on watched actor (actor, contactType)
- ITR:OnContactEnd - fires when physics contact ends on watched actor (actor, contactType)
- ITR:OnWitnessed - fires once per witness per crime (witness, perpetrator, crimeType, victim, detectionValue). Pickpocket reports the victim rather than the player as witness; murder excludes the victim. Filterable on witness/perpetrator/victim refs/formlists/factions and on crimeType equality. Trespass alarms are rate-limited to one event per witness per 3 seconds.
- ITR:OnImpactDataSpawn - fires when a projectile resolves ImpactData on a non-actor hit (impactData, x, y, z, normalX, normalY, normalZ, projectile, target, weapon, material)
- ITR:OnSprayDecal - fires per blood spray decal placement during limb sever/explode (impactData, x, y, z, normalX, normalY, normalZ)
- ITR:OnWoundSpray - fires per wound blood spray on an actor (actor, impactData, x, y, z, dx, dy, dz, hitLocation, source, weapon). Covers all three vanilla Actor::CreateBlood callers; third-party direct calls are not observed
- ITR:OnWeatherChange - fires when vanilla weather processing starts or completes a transition (phase: 0 start, 1 complete, nextWeather, previousWeather). Covers Sky::Update and both vanilla weather-command ForceWeather calls; third-party direct ForceWeather calls are not observed
- ITR:OnNearMiss - fires when a projectile passes near an actor without hitting (actor, shooter, weapon, distance). iCooldownMs has a 50ms floor
- ITR:OnCasinoBan - fires when the player is banned from a casino (casino)
- ITR:OnEffectApplied - fires when a magic effect is applied (target, magicItem, effectItemIndex, caster). magicItem is the parent spell/ingestible; effectItemIndex selects the effect within it
- ITR:OnEffectRemoved - fires when a magic effect is removed (target, magicItem, effectItemIndex, caster)
- ITR:OnPreFastTravel - fires before fast travel commits, after the confirm dialog (player, destinationMarker, destWorldspace - null for interiors, distance - straight-line engine units). Cancellable: SetFunctionValue 0 to veto. Catches map menu, script and TTW train travel. Travel-hours cost is deliberately not replicated, derive cost from distance
- ITR:OnPreStartCombat - fires synchronously when the vanilla StartCombat script command extracts its arguments successfully at runtime, before combat eligibility is evaluated (target). The event's calling reference is the actor on which StartCombat was called. It reports the command attempt even if combat does not begin
- ITR:OnPreWeaponSwitch - fires when combat AI decides to switch weapons (actor, proposedWeapon - 0 means unequip, currentWeapon - equipped at decision time). Cancellable: SetFunctionValue 0 to veto. The decision is deferred one AI tick while handlers run
- ITR:OnKnockdown - fires when an actor is knocked down (actor, cause: 1 force/impulse, 2 paralysis, 3 physics)
- ITR:OnGetUp - fires on get-up transitions (actor, phase: 0 animation begins, 1 fully upright)
- ITR:OnTileValueChange - fires when a watched tile trait changes (menuID, traitID, oldValue, newValue, watchId). Register watches with WatchTileValue. Reaction-driven recomputes also fire
- ITR:OnTileStrValueChange - fires when a watched string trait changes (menuID, traitID, oldStr, newStr, watchId)
- ITR:OnRadioTrackChange - fires once per radio output that attempts to start a track (filePath, wasInjected), queued as the path is handed to the engine rather than on confirmed playback. A single station advance reaches the world speakers with the `_mono` path and the pip-boy with the original path, so one advance on the tuned station fires twice with different paths. Repeats of the same path within a frame are collapsed, and at most 8 distinct paths are reported per frame
- ITR:OnPreDeath - fires when a non-essential actor is about to die, before the death commits (actor, killer). Cancellable: SetFunctionValue 0 to veto, which diverts the actor into the engine's own essential-down path (knocked down, recovers at engine-managed minimal health). Excludes player death, gib/dismember/explosion fatalities and VATS finishing moves. Vetoed kills still credit challenge counters and death perk entries, identical to vanilla essential behaviour
- ITR:OnPreHitDamage - fires when final HitData is handed to Actor::HitMe for an actual actor hit, after DT/DR, hit location and dmgMult are resolved but before the hit is processed (target, attacker, weapon, damage, hitLocation). It does not fire for VATS previews or other damage calculations that are never applied. Damage is final health damage, or final limb damage for weapon-condition hits. Mutable: SetFunctionValue a multiplier (0 negates, 0.5 halves, 2 doubles); multiple handlers multiply together. Scales health, limb and fatigue damage together
- ITR:OnPreHealthDamage - catch-all fired before any final health reduction applies (target, source, delta - negative, post-mitigation). Covers falls, traps, damage-over-time ticks (once per tick), scripted DamageAV, and weapon hits again post-mitigation. Mutable: SetFunctionValue a multiplier. For weapon-hit scaling prefer ITR:OnPreHitDamage
- WakeyWakeyNPC - fires per NPC woken by nearby player gunfire (actor). Legacy event name for third-party compatibility
- ITR:OnNPCWokeByGunfire - fires per NPC woken by nearby player gunfire (actor, shooter)
- ITR:OnDialogueMenuBuild - fires after the dialogue menu topic list (re)builds, on open and after each reply (speaker, topicCount). Add synthetic rows here with AddDialogueTopicEntry
- ITR:OnDialogueTopicSelected - fires when a synthetic dialogue row is clicked (speaker, syntheticId)
- ITR:OnVATSEnter - fires when VATS is entered (target)
- ITR:OnVATSLeave - fires when VATS is left (reason)
- ITR:OnKillCamStart - fires when a kill cam begins (target)
- ITR:OnKillCamEnd - fires when a kill cam ends (target)

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
- DoorPinchFix - temporarily disable non-load door collision after an open/close so actors aren't pinched
- CompanionNoBlock - stop the player and current companion from physically blocking each other
- ELMO - convert quest objectives and reputation popups to corner messages
- MessageBoxQuickClose - quick close message boxes with Enter/Space
- OwnerNameInfo - show item owner on crosshair prompt
- SaveFileSize - show save file size in save/load menu
- AutoQuickLoad - auto-load quicksave on main menu
- AutoGodMode - god mode on game start
- AltTabMute - mute audio when alt-tabbed
- ConsoleInputSuppression - suppress modded keyboard/controller binds while the console is open (typing, tilde close, Esc, screenshots and mouse ref-picking unaffected)
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
- AggroThreshold - NPCs absorb a configurable number of player hits before turning hostile (iAllyHit*/iFriendHit* thresholds, per-mode flags)
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
- GetLineOfSightCrashFix - fix crash from a null node in the line-of-sight check
- LockpickOwnerKarmaFix - no karma loss picking a lock you already own
- InlineGlyphFix - correct inline control-glyph sizing/placement in UI text
- ItemModFlagSafety - guard weapon mod flag writes against invalid entries. When JIP LN is present this patches a jnz inside JIP's GetEntryDataModFlagsHook, verified by byte signature - a future JIP update that reshapes that hook silently disables this feature (set bDebugLog=1 under [Debug] and check itr-nvse.log in the game folder)
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
| Tweaks | bVATSLimbFix | Force VATS limb hiding on for dismemberment integrations |
