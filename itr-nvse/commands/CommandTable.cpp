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
#include "commands/ChallengeCommands.h"
#include "commands/DialogueCommands.h"
#include "commands/ForceSayCommand.h"
#include "commands/IsSayingCommand.h"
#include "commands/WeaponEmissiveCommands.h"
#include "commands/ItemModFlagCommands.h"
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"
#include "commands/CommandBoundsCommand.h"
#include "commands/ToggleAllPrimitives.h"
#include "commands/PathingCommands.h"
#include "commands/HairColorCommands.h"
#include "commands/CasinoBanCommands.h"
#include "commands/ExteriorDoorCommands.h"
#include "commands/ContainerCommands.h"
#include "commands/HavokCommands.h"
#include "commands/WorldspaceOffsetCommands.h"
#include "commands/DetectionSoundCommands.h"
#include "commands/BarterCommands.h"
#include "commands/CenterOnCellAltCommand.h"
#include "features/CameraOverride.h"
#include "features/NoWeaponSearch.h"
#include "features/PreventWeaponSwitch.h"
#include "features/PerkRuntimeFramework.h"
#include "features/EyeMeshOverride.h"
#include "features/AimZoomFirstPersonOnly.h"
#include "handlers/DialogueCameraHandler.h"
#include "commands/GroundCommands.h"
#include "commands/GestureCommand.h"

extern void Log(const char* fmt, ...);

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
	ImperativeCommands::RegisterCommands(nvse); //IsRadioPlaying

	nvse->SetOpcodeBase(0x401D);
	CameraOverride::RegisterCommands(nvse); //SetCameraAngle

	nvse->SetOpcodeBase(0x401E);
	StringCommands::RegisterCommands(nvse); //Sv_TrimStr, Sv_Join, Sv_Reverse

	nvse->SetOpcodeBase(0x4021);
	ImperativeCommands::RegisterCommands2(nvse); //GetRefsSortedByDistance..GetTargetInitialLocation

	nvse->SetOpcodeBase(0x402A);
	NoWeaponSearch::RegisterCommands(nvse); //SetNoWeaponSearch, GetNoWeaponSearch

	nvse->SetOpcodeBase(0x402C);
	PreventWeaponSwitch::RegisterCommands(nvse); //SetPreventWeaponSwitch, GetPreventWeaponSwitch

	nvse->SetOpcodeBase(0x402E);
	RadioCommands::RegisterCommands(nvse); //GetPlayingRadioTrack, GetPlayingRadioTrackFileName

	nvse->SetOpcodeBase(0x4030);
	ImperativeCommands::RegisterCommands3(nvse); //UseAidItem

	nvse->SetOpcodeBase(0x4031);
	RadioCommands::RegisterCommands2(nvse); //GetPlayingRadioText

	nvse->SetOpcodeBase(0x4032);
	ImperativeCommands::RegisterCommands6(nvse); //ResurrectActorEx

	nvse->SetOpcodeBase(0x4034);
	ChallengeCommands::RegisterCommands(nvse); //ModChallenge

	nvse->SetOpcodeBase(0x4035);
	ImperativeCommands::RegisterCommands4(nvse); //SetCreatureCombatSkill, ResurrectAll, ForceReload

	nvse->SetOpcodeBase(0x4038);
	DialogueCommands::RegisterCommands(nvse); //GetDialogueInfoFlags, SetDialogueInfoFlags, GetDisplayedDialogueInfos

	nvse->SetOpcodeBase(0x403B);
	ImperativeCommands::RegisterCommands5(nvse); //SetRaceAlt

	nvse->SetOpcodeBase(0x403C);
	ForceSayCommand::RegisterCommands(nvse); //ForceSay

	nvse->SetOpcodeBase(0x4050);
	WeaponEmissiveCommands::RegisterCommands(nvse); //SetWeaponEmissiveColor, ClearWeaponEmissiveColor

	nvse->SetOpcodeBase(0x4052);
	UICommands::RegisterCommands(nvse); //SetUIAlphaMap

	nvse->SetOpcodeBase(0x4053);
	ActorValueCommands::RegisterCommands(nvse); //DamageActorValueAlt

	nvse->SetOpcodeBase(0x4054);
	IsSayingCommand::RegisterCommands(nvse); //IsSaying

	nvse->SetOpcodeBase(0x4055);
	DialogueCameraHandler::RegisterCommands(nvse); //SetDialogueCameraDolly, SetDialogueCameraShake

	nvse->SetOpcodeBase(0x4057);
	GroundCommands::RegisterCommands(nvse); //MoveToTerrain, GetDistanceToTerrain, MoveToGround, GetDistanceToGround

	nvse->SetOpcodeBase(0x405B);
	ImperativeCommands::RegisterCommands7(nvse); //ForceCrouch, DisableCrouching

	nvse->SetOpcodeBase(0x405F);
	ImperativeCommands::RegisterCommands8(nvse); //SetOnContactWatch, GetOnContactWatch

	nvse->SetOpcodeBase(0x4061);
	ImperativeCommands::RegisterCommands9(nvse); //ForceCombatTarget

	nvse->SetOpcodeBase(0x4062);
	DialogueCameraHandler::RegisterCommands2(nvse); //SetDialogueCameraEnabled, SetDialogueCameraMode, SetDialogueCameraFixedAngle, SetDialogueCameraAngle

	nvse->SetOpcodeBase(0x4066);
	ImperativeCommands::RegisterCommands10(nvse); //RefillAmmo

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
	UICommands::RegisterCommands2(nvse); //SetUITexOffset

	nvse->SetOpcodeBase(0x40AA);
	ExteriorDoorCommands::RegisterCommands2(nvse); //GetRefNextTeleportDoor

	nvse->SetOpcodeBase(0x40AB);
	ContainerCommands::RegisterCommands(nvse); //GetVisibleContainerInventoryCount

	nvse->SetOpcodeBase(0x40B0);
	HavokCommands::RegisterCommands(nvse); //IsRigidBodyAtRest

	nvse->SetOpcodeBase(0x40B1);
	PerkRuntimeFramework::RegisterCommands(nvse); //GetPerkEligibility..GetPerksForForm

	nvse->SetOpcodeBase(0x40B6);
	DetectionSoundCommands::RegisterCommands(nvse); //CreateDetectionSoundAt, CreateAnonymousDetectionSoundAt

	nvse->SetOpcodeBase(0x40B8);
	BarterCommands::RegisterCommands(nvse); //ShowBarterMenuWhitelist, ShowBarterMenuBlacklist

	nvse->SetOpcodeBase(0x40BA);
	HavokCommands::RegisterCommands2(nvse); //Ragdoll

	nvse->SetOpcodeBase(0x40BB);
	HavokCommands::RegisterCommands3(nvse); //RagdollLimb

	nvse->SetOpcodeBase(0x40BC);
	EyeMeshOverride::RegisterCommands(nvse); //SetEyeMesh, ClearEyeMesh

	nvse->SetOpcodeBase(0x40BF);
	AimZoomFirstPersonOnly::RegisterCommands(nvse); //SetAimZoomFirstPersonOnly, GetAimZoomFirstPersonOnly

	nvse->SetOpcodeBase(0x40C1);
	FakeHitHandler::RegisterCommands2(nvse); //FakeImpact

	nvse->SetOpcodeBase(0x40C2);
	CenterOnCellAltCommand::RegisterCommands(nvse); //CenterOnCellAlt

#ifdef _DEBUG
	nvse->SetOpcodeBase(0x4067);
	CommandBoundsCommand::RegisterCommands(nvse); //RunITRCommandBounds
#endif

	nvse->SetOpcodeBase(0x40AC);
	ItemModFlagCommands::RegisterCommands(nvse); //SetItemModFlags, GetItemModFlags

	nvse->SetOpcodeBase(0x410E);
	GestureCommand::RegisterCommands(nvse); //Gesture
}
