#pragma once

//centralised itr-nvse command registration - all itr-nvse opcodes visible in one place
//
// 0x4008  DisableKeyEx
// 0x4009  EnableKeyEx
// 0x4017  SetFallDamageMult
// 0x4018  GetFallDamageMult
// 0x4019  ClearFallDamageMult
// 0x401A  FakeHit
// 0x401B  FakeHitEx
// 0x401C  IsRadioPlaying
// 0x401D  SetCameraAngle
// 0x401E  Sv_TrimStr
// 0x401F  Sv_Join
// 0x4020  Sv_Reverse
// 0x4021  GetRefsSortedByDistance
// 0x4022  Duplicate
// 0x4023  GetAvailableRecipes
// 0x4024  ChangeRadioTrack
// 0x4025  DumpCombatTarget (_DEBUG only)
// 0x4026  GetTargetLastSeenLocation
// 0x4027  GetTargetDetectedLocation
// 0x4028  GetTargetLastFullyVisibleLocation
// 0x4029  GetTargetInitialLocation
// 0x402A  SetNoWeaponSearch
// 0x402B  GetNoWeaponSearch
// 0x402C  SetPreventWeaponSwitch
// 0x402D  GetPreventWeaponSwitch
// 0x402E  GetPlayingRadioTrack
// 0x402F  GetPlayingRadioTrackFileName
// 0x4030  UseAidItem
// 0x4031  GetPlayingRadioText
// 0x4032  ResurrectActorEx
// 0x4034  ModChallenge
// 0x4035  SetCreatureCombatSkill
// 0x4036  ResurrectAll
// 0x4037  ForceReload
// 0x4038  GetDialogueInfoFlags
// 0x4039  SetDialogueInfoFlags
// 0x403A  GetDisplayedDialogueInfos
// 0x403B  SetRaceAlt
// 0x403C  ForceSay
// 0x4050  SetWeaponEmissiveColor
// 0x4051  ClearWeaponEmissiveColor
// 0x4052  SetUIAlphaMap
// 0x4053  DamageActorValueAlt
// 0x4054  IsSaying
// 0x4055  SetDialogueCameraDolly
// 0x4056  SetDialogueCameraShake
// 0x4057  MoveToTerrain
// 0x4058  GetDistanceToTerrain
// 0x4059  MoveToGround
// 0x405A  GetDistanceToGround
// 0x405B  ForceCrouch
// 0x405C  DisableCrouching
// 0x405F  SetOnContactWatch
// 0x4060  GetOnContactWatch
// 0x4061  ForceCombatTarget
// 0x4062  SetDialogueCameraEnabled
// 0x4063  SetDialogueCameraMode
// 0x4064  SetDialogueCameraFixedAngle
// 0x4065  SetDialogueCameraAngle
// 0x4066  RefillAmmo
// 0x4067  RunITRCommandBounds (Debug only)
// 0x4068  ToggleAllPrimitives (TAP)
// 0x409C  GetRefExteriorDoor
// 0x409D  GetWorldspaceOffsetX
// 0x409E  GetWorldspaceOffsetY
// 0x409F  GetWorldspaceOffsetScale
// 0x40A0  CanPathToRef
// 0x40A1  GetPathDistanceToRef
// 0x40A2  GetPathNodeCount
// 0x40A3  GetNthPathNode
// 0x40A4  GetPathToRef
// 0x40A5  SetHairColorAlt
// 0x40A6  GetHairColorAlt
// 0x40A7  SetCasinoBan
// 0x40A8  GetCasinoBan
// 0x40A9  SetUITexOffset
// 0x40AA  GetRefNextTeleportDoor
// 0x40B0  IsRigidBodyAtRest
// 0x40B1  GetPerkEligibility
// 0x40B2  GetPerkBlockers
// 0x40B3  GetEligiblePerks
// 0x40B4  GetPerkConditions
// 0x40B5  GetPerksForForm
// 0x410E  Gesture
//
// not registered by itr-nvse source: 0x4000-0x4007, 0x400A-0x4016, 0x4033,
// 0x403D-0x404F, 0x405D-0x405E, 0x4069-0x409B, 0x40AB-0x40AF,
// 0x40B6-0x410D, 0x410F
// These are not automatically free; other local plugins use slots inside the
// Into the Rough-owned blocks. Check /mnt/d/plugins/opcodes.txt before assigning.

void RegisterAllCommands(void* nvse);
