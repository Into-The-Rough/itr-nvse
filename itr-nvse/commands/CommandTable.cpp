//centralised itr-nvse command registration
//each *_Init still queries interfaces and registers commands sequentially,
//but ALL itr-nvse SetOpcodeBase calls are here so the plugin map is visible

#include "CommandTable.h"
#include "nvse/PluginAPI.h"

#include "handlers/OnKeyStateHandler.h"
#include "handlers/FallDamageHandler.h"
#include "handlers/FakeHitHandler.h"
#include "commands/ImperativeCommands.h"
#include "commands/StringCommands.h"
#include "commands/RadioCommands.h"
#include "commands/ResurrectCommands.h"
#include "commands/ForceCombatTargetCommands.h"
#include "commands/CrouchCommands.h"
#include "commands/ChallengeCommands.h"
#include "commands/DialogueCommands.h"
#include "commands/WeaponEmissiveCommands.h"
#include "commands/ItemModFlagCommands.h"
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"
#include "commands/ToggleAllPrimitives.h"
#include "commands/PathingCommands.h"
#include "commands/HairColorCommands.h"
#include "commands/CasinoBanCommands.h"
#include "commands/ExteriorDoorCommands.h"
#include "commands/ContainerCommands.h"
#include "commands/HavokCommands.h"
#include "commands/LimbCommands.h"
#include "commands/PlayerGrabCommands.h"
#include "commands/WorldspaceOffsetCommands.h"
#include "commands/DetectionSoundCommands.h"
#include "commands/AlertCommands.h"
#include "commands/BarterCommands.h"
#include "commands/CenterOnCellAltCommand.h"
#include "commands/StartNewGameCommand.h"
#include "features/CameraOverride.h"
#include "features/NoWeaponSearch.h"
#include "features/PreventWeaponSwitch.h"
#include "commands/PerkCommands.h"
#include "features/EyeMeshOverride.h"
#include "features/RaceEyeCommands.h"
#include "features/AimZoomFirstPersonOnly.h"
#include "handlers/DialogueCameraHandler.h"
#include "commands/GroundCommands.h"
#include "commands/GestureCommand.h"
#include "commands/NavmeshCommands.h"
#include "commands/CoverCommands.h"
#include "commands/ParticleCommands.h"
#include "features/RadioInjection.h"
#include "features/ThrowArcPreview.h"
#include "handlers/OnTileValueChangeHandler.h"
#include "internal/globals.h"

void RegisterAllCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	nvse->SetOpcodeBase(0x4008);
	OnKeyStateHandler::RegisterCommands(nvse); //DisableKeyEx, EnableKeyEx

	nvse->SetOpcodeBase(0x4017);
	FallDamageHandler::RegisterCommands(nvse); //SetFallDamageMult, GetFallDamageMult, ClearFallDamageMult

	nvse->SetOpcodeBase(0x401A);
	FakeHitHandler::RegisterCommands(nvse); //FakeHit, FakeHitEx

	nvse->SetOpcodeBase(0x401C);
	RadioCommands::RegisterIsPlayingCommands(nvse); //IsRadioPlaying

	nvse->SetOpcodeBase(0x401D);
	CameraOverride::RegisterCommands(nvse); //SetCameraAngle

	nvse->SetOpcodeBase(0x401E);
	StringCommands::RegisterCommands(nvse); //Sv_TrimStr, Sv_Join, Sv_Reverse

	nvse->SetOpcodeBase(0x4021);
	ImperativeCommands::RegisterCommands(nvse); //GetRefsSortedByDistance, Duplicate, GetAvailableRecipes
	RadioCommands::RegisterChangeTrackCommands(nvse); //ChangeRadioTrack
	ImperativeCommands::RegisterTargetLocationCommands(nvse); //GetTargetLastSeenLocation..GetTargetInitialLocation

	nvse->SetOpcodeBase(0x402A);
	NoWeaponSearch::RegisterCommands(nvse); //SetNoWeaponSearch, GetNoWeaponSearch

	nvse->SetOpcodeBase(0x402C);
	PreventWeaponSwitch::RegisterCommands(nvse); //SetPreventWeaponSwitch, GetPreventWeaponSwitch

	nvse->SetOpcodeBase(0x402E);
	RadioCommands::RegisterCommands(nvse); //GetPlayingRadioTrack, GetPlayingRadioTrackFileName

	nvse->SetOpcodeBase(0x4030);
	ImperativeCommands::RegisterAidCommands(nvse); //UseAidItem

	nvse->SetOpcodeBase(0x4031);
	RadioCommands::RegisterRadioTextCommands(nvse); //GetPlayingRadioText

	nvse->SetOpcodeBase(0x4032);
	ResurrectCommands::RegisterCommands(nvse); //ResurrectActorEx

	nvse->SetOpcodeBase(0x4034);
	ChallengeCommands::RegisterCommands(nvse); //ModChallenge

	nvse->SetOpcodeBase(0x4035);
	ImperativeCommands::RegisterCombatSkillCommands(nvse); //SetCreatureCombatSkill
	ResurrectCommands::RegisterResurrectAllCommands(nvse); //ResurrectAll
	ImperativeCommands::RegisterForceReloadCommands(nvse); //ForceReload

	nvse->SetOpcodeBase(0x4038);
	DialogueCommands::RegisterCommands(nvse); //GetDialogueInfoFlags, SetDialogueInfoFlags, GetDisplayedDialogueInfos

	nvse->SetOpcodeBase(0x403B);
	ImperativeCommands::RegisterSetRaceCommands(nvse); //SetRaceAlt

	nvse->SetOpcodeBase(0x403C);
	DialogueCommands::RegisterForceSayCommands(nvse); //ForceSay

	nvse->SetOpcodeBase(0x4050);
	WeaponEmissiveCommands::RegisterCommands(nvse); //SetWeaponEmissiveColor, ClearWeaponEmissiveColor

	nvse->SetOpcodeBase(0x4052);
	UICommands::RegisterCommands(nvse); //SetUIAlphaMap

	nvse->SetOpcodeBase(0x4053);
	ActorValueCommands::RegisterCommands(nvse); //DamageActorValueAlt

	nvse->SetOpcodeBase(0x4054);
	DialogueCommands::RegisterIsSayingCommands(nvse); //IsSaying

	nvse->SetOpcodeBase(0x4055);
	DialogueCameraHandler::RegisterCommands(nvse); //SetDialogueCameraDolly, SetDialogueCameraShake

	nvse->SetOpcodeBase(0x4057);
	GroundCommands::RegisterCommands(nvse); //MoveToTerrain, GetDistanceToTerrain, MoveToGround, GetDistanceToGround

	nvse->SetOpcodeBase(0x405B);
	CrouchCommands::RegisterCommands(nvse); //ForceCrouch, DisableCrouching

	nvse->SetOpcodeBase(0x405F);
	ImperativeCommands::RegisterContactWatchCommands(nvse); //SetOnContactWatch, GetOnContactWatch

	nvse->SetOpcodeBase(0x4061);
	ForceCombatTargetCommands::RegisterCommands(nvse); //ForceCombatTarget

	nvse->SetOpcodeBase(0x4062);
	DialogueCameraHandler::RegisterModeCommands(nvse); //SetDialogueCameraEnabled, SetDialogueCameraMode, SetDialogueCameraFixedAngle, SetDialogueCameraAngle

	nvse->SetOpcodeBase(0x4066);
	ImperativeCommands::RegisterRefillAmmoCommands(nvse); //RefillAmmo

	nvse->SetOpcodeBase(0x4068);
	ToggleAllPrimitives::RegisterCommands(nvse); //ToggleAllPrimitives

	nvse->SetOpcodeBase(0x409C);
	ExteriorDoorCommands::RegisterCommands(nvse); //GetRefExteriorDoor

	nvse->SetOpcodeBase(0x409D);
	WorldspaceOffsetCommands::RegisterCommands(nvse); //GetWorldspaceOffsetX..GetWorldspaceOffsetScale

	nvse->SetOpcodeBase(0x40A0);
	PathingCommands::RegisterCommands(nvse); //CanPathToRef..GetPathToRef

	nvse->SetOpcodeBase(0x40A5);
	HairColorCommands::RegisterCommands(nvse); //SetHairColorAlt, GetHairColorAlt

	nvse->SetOpcodeBase(0x40A7);
	CasinoBanCommands::RegisterCommands(nvse); //SetCasinoBan, GetCasinoBan

	nvse->SetOpcodeBase(0x40A9);
	UICommands::RegisterTexOffsetCommands(nvse); //SetUITexOffset

	nvse->SetOpcodeBase(0x40AA);
	ExteriorDoorCommands::RegisterNextTeleportCommands(nvse); //GetRefNextTeleportDoor

	nvse->SetOpcodeBase(0x40AB);
	ContainerCommands::RegisterCommands(nvse); //GetVisibleContainerInventoryCount

	nvse->SetOpcodeBase(0x40B0);
	HavokCommands::RegisterCommands(nvse); //IsRigidBodyAtRest

	nvse->SetOpcodeBase(0x40B1);
	PerkCommands::RegisterCommands(nvse); //GetPerkEligibility..GetPerksForForm

	nvse->SetOpcodeBase(0x40B6);
	DetectionSoundCommands::RegisterCommands(nvse); //CreateDetectionSoundAt, CreateAnonymousDetectionSoundAt

	nvse->SetOpcodeBase(0x40B8);
	BarterCommands::RegisterCommands(nvse); //ShowBarterMenuWhitelist, ShowBarterMenuBlacklist

	nvse->SetOpcodeBase(0x40BA);
	HavokCommands::RegisterRagdollCommands(nvse); //Ragdoll

	nvse->SetOpcodeBase(0x40BB);
	HavokCommands::RegisterRagdollLimbCommands(nvse); //RagdollLimb

	nvse->SetOpcodeBase(0x40BC);
	EyeMeshOverride::RegisterCommands(nvse); //SetEyeMesh, ClearEyeMesh

	nvse->SetOpcodeBase(0x40BF);
	AimZoomFirstPersonOnly::RegisterCommands(nvse); //SetAimZoomFirstPersonOnly, GetAimZoomFirstPersonOnly

	nvse->SetOpcodeBase(0x40C1);
	FakeHitHandler::RegisterImpactCommands(nvse); //FakeImpact

	nvse->SetOpcodeBase(0x40C2);
	CenterOnCellAltCommand::RegisterCommands(nvse); //CenterOnCellAlt

#ifdef _DEBUG
	nvse->SetOpcodeBase(0x4029);
	ImperativeCommands::RegisterDebugCommands(nvse); //DumpCombatTarget
#endif

	nvse->SetOpcodeBase(0x40AC);
	ItemModFlagCommands::RegisterCommands(nvse); //SetItemModFlags, GetItemModFlags

	nvse->SetOpcodeBase(0x410E);
	GestureCommand::RegisterCommands(nvse); //Gesture

	nvse->SetOpcodeBase(0x40C3);
	DialogueCameraHandler::RegisterAngleEnableCommands(nvse); //SetDialogueCameraAngleEnabled

	nvse->SetOpcodeBase(0x40C4);
	RaceEyeCommands::RegisterCommands(nvse); //AddRaceEye, RemoveRaceEye, ClearRaceEyes

	nvse->SetOpcodeBase(0x40BE);
	StartNewGameCommand::RegisterCommands(nvse); //StartNewGame

	//0x4110+ is externally reserved (Float Prices, CacheUI) per opcodes.txt, these live in the free 0x40C7-0x40FF block
	nvse->SetOpcodeBase(0x40C7);
	RadioInjection::RegisterCommands(nvse); //PlayRadioFile, QueueRadioTrack, ClearRadioQueue, SetRadioQueueLoop, GetRadioQueueSize

	nvse->SetOpcodeBase(0x40CC);
	NavmeshCommands::RegisterCommands(nvse); //GetPathLength, IsPointOnNavmesh, GetCoverPointsInRadius, GetBestCoverFromThreat

	nvse->SetOpcodeBase(0x40D0);
	DialogueCommands::RegisterTopicCommands(nvse); //AddDialogueTopicEntry, SetDialogueTopicEntryAlpha, SetDialogueTopicHidden, SetDialogueTopicOrder, ClearDialogueTopicOverrides

	nvse->SetOpcodeBase(0x40D5);
	OnTileValueChangeHandler::RegisterCommands(nvse); //WatchTileValue, UnwatchTileValue

	nvse->SetOpcodeBase(0x40D9);
	AlertCommands::RegisterCommands(nvse); //SetAlertNS, GetAlertNS

	nvse->SetOpcodeBase(0x40DB);
	HavokCommands::RegisterLimbVelocityCommands(nvse); //SetDismemberedLimbVelocity

	nvse->SetOpcodeBase(0x40DC);
	CoverCommands::RegisterCommands(nvse); //GetActorCoverState, GetActorCoverInfo

	nvse->SetOpcodeBase(0x40DE);
	ParticleCommands::RegisterCommands(nvse); //SpawnTempParticle

	nvse->SetOpcodeBase(0x40E0);
	PerkCommands::RegisterEligibleExCommands(nvse);

	nvse->SetOpcodeBase(0x40E2);
	DialogueCommands::RegisterCurrentDialogueCommands(nvse); //GetCurrentDialogue

	nvse->SetOpcodeBase(0x40E6);
	LimbCommands::RegisterCommands(nvse); //GetLimbGoneType

	nvse->SetOpcodeBase(0x40E7);
	HavokCommands::RegisterCenterOfMassCommands(nvse); //GetRigidBodyCenterOfMass, SetRigidBodyCenterOfMass

	nvse->SetOpcodeBase(0x40F0);
	PlayerGrabCommands::RegisterCommands(nvse); //ForcePlayerGrab, ReleasePlayerGrab

	nvse->SetOpcodeBase(0x40F2);
	ThrowArcPreview::RegisterCommands(nvse); //StartThrowArcPreview, StopThrowArcPreview
}
