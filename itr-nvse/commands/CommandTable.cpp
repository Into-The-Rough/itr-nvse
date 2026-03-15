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
#include "commands/WeaponEmissiveCommands.h"
#include "commands/UICommands.h"
#include "commands/ActorValueCommands.h"
#include "features/CameraOverride.h"
#include "features/NoWeaponSearch.h"
#include "features/PreventWeaponSwitch.h"

extern void Log(const char* fmt, ...);

void RegisterAllCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	/*4008*/ nvse->SetOpcodeBase(0x4008);
	OKSH_RegisterCommands(nvse);                   //DisableKeyEx, EnableKeyEx

	/*4017*/ nvse->SetOpcodeBase(0x4017);
	FDH_RegisterCommands(nvse);                    //SetFallDamageMult, GetFallDamageMult, ClearFallDamageMult

	/*401A*/ nvse->SetOpcodeBase(0x401A);
	FakeHit_RegisterCommands(nvse);                //FakeHit, FakeHitEx

	/*401C*/ nvse->SetOpcodeBase(0x401C);
	ImperativeCommands_RegisterCommands(nvse);     //IsRadioPlaying

	/*401D*/ nvse->SetOpcodeBase(0x401D);
	CameraOverride_RegisterCommands(nvse);         //SetCameraAngle

	/*401E*/ nvse->SetOpcodeBase(0x401E);
	StringCommands_RegisterCommands(nvse);         //Sv_TrimStr, Sv_Join, Sv_Reverse

	/*4021*/ nvse->SetOpcodeBase(0x4021);
	ImperativeCommands_RegisterCommands2(nvse);    //GetRefsSortedByDistance..GetTargetInitialLocation

	/*402A*/ nvse->SetOpcodeBase(0x402A);
	NoWeaponSearch_RegisterCommands(nvse);         //SetNoWeaponSearch, GetNoWeaponSearch

	/*402C*/ nvse->SetOpcodeBase(0x402C);
	PreventWeaponSwitch_RegisterCommands(nvse);    //SetPreventWeaponSwitch, GetPreventWeaponSwitch

	/*402E*/ nvse->SetOpcodeBase(0x402E);
	RadioCommands_RegisterCommands(nvse);          //GetPlayingRadioTrack, GetPlayingRadioTrackFileName

	/*4030*/ nvse->SetOpcodeBase(0x4030);
	ImperativeCommands_RegisterCommands3(nvse);    //UseAidItem

	/*4031*/ nvse->SetOpcodeBase(0x4031);
	RadioCommands_RegisterCommands2(nvse);         //GetPlayingRadioText

	/*4034*/ nvse->SetOpcodeBase(0x4034);
	ChallengeCommands_RegisterCommands(nvse);      //ModChallenge

	/*4035*/ nvse->SetOpcodeBase(0x4035);
	ImperativeCommands_RegisterCommands4(nvse);    //SetCreatureCombatSkill, ResurrectAll, ForceReload

	/*4038*/ nvse->SetOpcodeBase(0x4038);
	DialogueCommands_RegisterCommands(nvse);       //GetDialogueInfoFlags, SetDialogueInfoFlags, GetDisplayedDialogueInfos

	/*403B*/ nvse->SetOpcodeBase(0x403B);
	ImperativeCommands_RegisterCommands5(nvse);    //SetRaceAlt

	/*4050*/ nvse->SetOpcodeBase(0x4050);
	WeaponEmissiveCommands_RegisterCommands(nvse); //SetWeaponEmissiveColor, ClearWeaponEmissiveColor

	/*4052*/ nvse->SetOpcodeBase(0x4052);
	UICommands_RegisterCommands(nvse);             //SetUIAlphaMap

	/*4053*/ nvse->SetOpcodeBase(0x4053);
	ActorValueCommands_RegisterCommands(nvse);     //DamageActorValueAlt

	Log("All commands registered (0x4008-0x4053)");
}
