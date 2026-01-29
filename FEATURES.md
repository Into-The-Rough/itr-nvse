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
- ClampToGround - clamp reference to ground

**Fall Damage**
- SetFallDamageMult - set fall damage multiplier per actor
- GetFallDamageMult - get fall damage multiplier
- ClearFallDamageMult - clear fall damage multiplier

**Combat**
- FakeHit - simulate hit on actor
- FakeHitEx - simulate hit with extended params
- SetNoWeaponSearch - disable weapon search for actor
- GetNoWeaponSearch - get weapon search state

**Camera**
- SetCameraAngle - direct camera transform control

**Weapon Switch**
- SetPreventWeaponSwitch - prevent NPC weapon switching
- GetPreventWeaponSwitch - get weapon switch prevention state

**Input**
- RegisterKeyHeld - register key held callback
- RegisterControlHeld - register control held callback
- UnregisterKeyHeld - unregister key held callback
- UnregisterControlHeld - unregister control held callback
- RegisterKeyDoubleTap - register double tap callback
- RegisterControlDoubleTap - register control double tap callback
- UnregisterKeyDoubleTap - unregister double tap callback
- UnregisterControlDoubleTap - unregister control double tap callback
- DisableKeyEx - disable key with handler
- EnableKeyEx - enable key with handler

## Event Handlers

- SetOnStealEventHandler - fires when items stolen
- SetOnWeaponJamEventHandler - fires when weapon jams
- SetOnWeaponDropEventHandler - fires when actor drops weapon
- SetOnFrenzyEventHandler - fires when actor frenzied
- SetOnConsoleOpenEventHandler - fires on console open
- SetOnConsoleCloseEventHandler - fires on console close
- SetOnCombatProcedureStartEventHandler - fires on combat AI change
- SetOnEntryPointEventHandler - fires when perk entry points execute
- SetOnSoundPlayedEventHandler - fires on any sound playback
- SetCornerMessageHandler - fires on HUD corner message
- SetOnDialogueTextEventHandler - fires on dialogue text display
- SetOnKeyDisabledEventHandler - fires when key disabled
- SetOnKeyEnabledEventHandler - fires when key enabled

## Features

- ELMO - objective/reputation popups as corner messages
- LocationVisitPopup - notification on revisiting locations
- MessageBoxQuickClose - quick close message boxes
- QuickDrop - drop weapon with hotkey
- Quick180 - rotate player 180 degrees instantly
- QuickReadNote - view notes on pickup without pip-boy
- PreventWeaponSwitch - stop NPCs switching weapons mid-combat
- VATSExtender - more VATS targets beyond vanilla limit
- CameraOverride - direct camera transform control
- AltTabMute - mute audio when alt-tabbed
- AutoGodMode - god mode on game start
- AutoQuickLoad - auto-load quicksave on main menu
- OwnerNameInfo - show item owner on crosshair
- SaveFileSize - show save file size in menu

## Fixes

- ArmorDTDRFix - NPC armor DT/DR cache update
- AshPileNames - show NPC name on ash piles
- ExplodingPantsFix - no explosions from worn projectiles
- FriendlyFire - player/ally friendly fire
- KillActorXPFix - no XP for kill command on dead
- NoDoorFade - no actor fade on door enter
- OwnedBeds - sleep in owned beds with consequences
- ReversePickpocketNoKarmaFix - no karma loss for reverse pickpocket
- SlowMotionPhysicsFix - stable ragdoll physics in VATS
- VATSLimbFix - hide dismembered limbs in VATS
- VATSProjectileFix - fix projectile hit chance in VATS

## Configuration

INI at `Data/config/itr-nvse.ini`, reload with `ReloadPluginConfig` console command.
