//centralised command registration
//each *_Init still queries interfaces and registers commands sequentially,
//but ALL SetOpcodeBase calls are here so the full map is visible

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
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"
#include "commands/CommandBoundsCommand.h"
#include "commands/ToggleAllPrimitives.h"
#include "commands/PathingCommands.h"
#include "commands/HairColorCommands.h"
#include "commands/CasinoBanCommands.h"
#include "features/CameraOverride.h"
#include "features/NoWeaponSearch.h"
#include "features/PreventWeaponSwitch.h"
#include "handlers/DialogueCameraHandler.h"
#include "commands/GroundCommands.h"
#include "commands/GestureCommand.h"

extern void Log(const char* fmt, ...);

void RegisterAllCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	/*4008*/ nvse->SetOpcodeBase(0x4008);
	OnKeyStateHandler::RegisterCommands(nvse);                   //DisableKeyEx, EnableKeyEx

	/*4017*/ nvse->SetOpcodeBase(0x4017);
	FallDamageHandler::RegisterCommands(nvse);                    //SetFallDamageMult, GetFallDamageMult, ClearFallDamageMult

	/*401A*/ nvse->SetOpcodeBase(0x401A);
	FakeHitHandler::RegisterCommands(nvse);                //FakeHit, FakeHitEx

	/*401C*/ nvse->SetOpcodeBase(0x401C);
	ImperativeCommands::RegisterCommands(nvse);     //IsRadioPlaying

	/*401D*/ nvse->SetOpcodeBase(0x401D);
	CameraOverride::RegisterCommands(nvse);         //SetCameraAngle

	/*401E*/ nvse->SetOpcodeBase(0x401E);
	StringCommands::RegisterCommands(nvse);         //Sv_TrimStr, Sv_Join, Sv_Reverse

	/*4021*/ nvse->SetOpcodeBase(0x4021);
	ImperativeCommands::RegisterCommands2(nvse);    //GetRefsSortedByDistance..GetTargetInitialLocation

	/*402A*/ nvse->SetOpcodeBase(0x402A);
	NoWeaponSearch::RegisterCommands(nvse);         //SetNoWeaponSearch, GetNoWeaponSearch

	/*402C*/ nvse->SetOpcodeBase(0x402C);
	PreventWeaponSwitch::RegisterCommands(nvse);    //SetPreventWeaponSwitch, GetPreventWeaponSwitch

	/*402E*/ nvse->SetOpcodeBase(0x402E);
	RadioCommands::RegisterCommands(nvse);          //GetPlayingRadioTrack, GetPlayingRadioTrackFileName

	/*4030*/ nvse->SetOpcodeBase(0x4030);
	ImperativeCommands::RegisterCommands3(nvse);    //UseAidItem

	/*4031*/ nvse->SetOpcodeBase(0x4031);
	RadioCommands::RegisterCommands2(nvse);         //GetPlayingRadioText

	/*4032*/ nvse->SetOpcodeBase(0x4032);
	ImperativeCommands::RegisterCommands6(nvse);    //ResurrectActorEx

	/*4034*/ nvse->SetOpcodeBase(0x4034);
	ChallengeCommands::RegisterCommands(nvse);      //ModChallenge

	/*4035*/ nvse->SetOpcodeBase(0x4035);
	ImperativeCommands::RegisterCommands4(nvse);    //SetCreatureCombatSkill, ResurrectAll, ForceReload

	/*4038*/ nvse->SetOpcodeBase(0x4038);
	DialogueCommands::RegisterCommands(nvse);       //GetDialogueInfoFlags, SetDialogueInfoFlags, GetDisplayedDialogueInfos

	/*403B*/ nvse->SetOpcodeBase(0x403B);
	ImperativeCommands::RegisterCommands5(nvse);    //SetRaceAlt

	/*403C*/ nvse->SetOpcodeBase(0x403C);
	ForceSayCommand::RegisterCommands(nvse);        //ForceSay

	/*4050*/ nvse->SetOpcodeBase(0x4050);
	WeaponEmissiveCommands::RegisterCommands(nvse); //SetWeaponEmissiveColor, ClearWeaponEmissiveColor

	/*4052*/ nvse->SetOpcodeBase(0x4052);
	UICommands::RegisterCommands(nvse);             //SetUIAlphaMap

	/*4053*/ nvse->SetOpcodeBase(0x4053);
	ActorValueCommands::RegisterCommands(nvse);     //DamageActorValueAlt

	/*4054*/ nvse->SetOpcodeBase(0x4054);
	IsSayingCommand::RegisterCommands(nvse);        //IsSaying

	/*4055*/ nvse->SetOpcodeBase(0x4055);
	DialogueCameraHandler::RegisterCommands(nvse);                    //SetDialogueCameraDolly, SetDialogueCameraShake

	/*4057*/ nvse->SetOpcodeBase(0x4057);
	GroundCommands::RegisterCommands(nvse);                           //MoveToTerrain, GetDistanceToTerrain, MoveToGround, GetDistanceToGround

	/*405B*/ nvse->SetOpcodeBase(0x405B);
	ImperativeCommands::RegisterCommands7(nvse);                     //ForceCrouch, DisableCrouching

	/*405F*/ nvse->SetOpcodeBase(0x405F);
	ImperativeCommands::RegisterCommands8(nvse);                     //SetOnContactWatch, GetOnContactWatch

	/*4061*/ nvse->SetOpcodeBase(0x4061);
	ImperativeCommands::RegisterCommands9(nvse);                     //ForceCombatTarget

	/*4062*/ nvse->SetOpcodeBase(0x4062);
	DialogueCameraHandler::RegisterCommands2(nvse);                  //SetDialogueCameraEnabled, SetDialogueCameraMode, SetDialogueCameraFixedAngle, SetDialogueCameraAngle

	/*4066*/ nvse->SetOpcodeBase(0x4066);
	ImperativeCommands::RegisterCommands10(nvse);                    //RefillAmmo

	/*4068*/ nvse->SetOpcodeBase(0x4068);
	ToggleAllPrimitives::RegisterCommands(nvse);                    //ToggleAllPrimitives

	/*40A0*/ nvse->SetOpcodeBase(0x40A0);
	PathingCommands::RegisterCommands(nvse);                        //CanPathToRef..GetPathToRef

	/*40A5*/ nvse->SetOpcodeBase(0x40A5);
	HairColorCommands::RegisterCommands(nvse);                      //SetHairColorAlt, GetHairColorAlt

	/*40A7*/ nvse->SetOpcodeBase(0x40A7);
	CasinoBanCommands::RegisterCommands(nvse);                      //SetCasinoBan, GetCasinoBan

#ifdef _DEBUG
	/*4067*/ nvse->SetOpcodeBase(0x4067);
	CommandBoundsCommand::RegisterCommands(nvse);                    //RunITRCommandBounds
#endif

	/*410E*/ nvse->SetOpcodeBase(0x410E);
	GestureCommand::RegisterCommands(nvse);                          //Gesture
}
