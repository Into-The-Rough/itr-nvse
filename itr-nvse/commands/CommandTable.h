#pragma once

//centralised command registration - all opcodes visible in one place
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
// 0x4025  DumpCombatTarget
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
//
// 0x405B  ForceCrouch
// 0x405C  DisableCrouching
// 0x405D-0x405E  SoundFilteringSoftware (SetActorSoundFilter, GetActorSoundFilter)
// 0x405F  SetOnContactWatch
// 0x4060  GetOnContactWatch
// 0x4061  ForceCombatTarget
// gaps in itr-nvse itself: 0x4033, 0x403D-0x404F
// next free itr-nvse-local slot: 0x4062
// shared suite occupancy for borrowed opcodes is tracked in /mnt/d/plugins/opcodes.txt

void RegisterAllCommands(void* nvse);
