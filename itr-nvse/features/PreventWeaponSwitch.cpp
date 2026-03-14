//prevents actors from switching weapons during combat
//hooks CombatProcedureSwitchWeapon::Update and finishes it early if actor is blocked

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include "internal/Detours.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"

#include "internal/globals.h"
#include "internal/ScopedLock.h"

//local ExtractArgs, different from NVSEScriptInterface version
#define EXTRACT_ARGS paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList
typedef bool (*ExtractArgs_t)(ParamInfo*, void*, UInt32*, TESObjectREFR*, TESObjectREFR*, Script*, ScriptEventList*, ...);
static ExtractArgs_t ExtractArgs = (ExtractArgs_t)0x5ACCB0;

static const int MAX_BLOCKED = 64;
static UInt32 g_blocked[MAX_BLOCKED] = {0};
static int g_count = 0;
static CRITICAL_SECTION g_lock;
static volatile LONG g_lockInit = 0;

static void EnsureLockInit()
{
	InitCriticalSectionOnce(&g_lockInit, &g_lock);
}

static bool IsBlocked_Unlocked(UInt32 refID)
{
	for (int i = 0; i < g_count; i++)
		if (g_blocked[i] == refID)
			return true;
	return false;
}

static bool IsBlocked(UInt32 refID)
{
	ScopedLock lock(&g_lock);
	return IsBlocked_Unlocked(refID);
}

static void SetBlocked(UInt32 refID, bool block)
{
	ScopedLock lock(&g_lock);
	if (block)
	{
		for (int i = 0; i < g_count; i++)
			if (g_blocked[i] == refID) return;
		if (g_count < MAX_BLOCKED)
			g_blocked[g_count++] = refID;
	}
	else
	{
		for (int i = 0; i < g_count; i++)
		{
			if (g_blocked[i] == refID)
			{
				g_blocked[i] = g_blocked[--g_count];
				g_blocked[g_count] = 0;
				break;
			}
		}
	}
}

//0x9DA7C0 = CombatProcedureSwitchWeapon::Update
typedef void (__thiscall *SwitchWeaponUpdate_t)(void* procedure);
static Detours::JumpDetour s_detour;


void __fastcall Hook_SwitchWeaponUpdate(void* procedure, void* edx)
{
	bool shouldBlock = false;
	{
		ScopedLock lock(&g_lock);
		if (g_count > 0)
		{
			void* controller = *(void**)((char*)procedure + 0x4);
			if (controller)
			{
				Actor* actor = (Actor*)Engine::CombatController_GetPackageOwner(controller);
				if (actor)
				{
					UInt32 refID = *(UInt32*)((char*)actor + 0x0C);
					if (IsBlocked_Unlocked(refID))
						shouldBlock = true;
				}
			}
		}
	}

	if (shouldBlock)
	{
		//eStatus = 2 (finished) at offset 0x8
		*(UInt32*)((char*)procedure + 0x8) = 2;
		return;
	}
	s_detour.GetTrampoline<SwitchWeaponUpdate_t>()(procedure);
}

//prologue: 6 bytes
void PreventWeaponSwitch_Init()
{
	EnsureLockInit();
	if (!s_detour.WriteRelJump(0x9DA7C0, Hook_SwitchWeaponUpdate, 6))
		Log("ERROR: PreventWeaponSwitch hook failed");
}

//vtable index 0x100
inline bool IsActorRef(TESObjectREFR* ref) {
	if (!ref) return false;
	UInt32 vtable = *(UInt32*)ref;
	UInt32 isActorFn = *(UInt32*)(vtable + 0x100);
	return ((bool (__thiscall*)(TESObjectREFR*))isActorFn)(ref);
}

static bool Cmd_SetPreventWeaponSwitch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 block = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &block))
		return true;

	if (IsActorRef(thisObj))
	{
		UInt32 refID = *(UInt32*)((char*)thisObj + 0x0C);
		SetBlocked(refID, block != 0);
		*result = 1;
	}
	return true;
}

static bool Cmd_GetPreventWeaponSwitch_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (IsActorRef(thisObj))
	{
		UInt32 refID = *(UInt32*)((char*)thisObj + 0x0C);
		*result = IsBlocked(refID) ? 1 : 0;
	}
	return true;
}

static ParamInfo kParams_SetPreventWeaponSwitch[1] = {
	{"block", kParamType_Integer, 0}
};

static CommandInfo kCommandInfo_SetPreventWeaponSwitch = {
	"SetPreventWeaponSwitch", "", 0, "Prevent actor from switching weapons",
	1, 1, kParams_SetPreventWeaponSwitch, Cmd_SetPreventWeaponSwitch_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_GetPreventWeaponSwitch = {
	"GetPreventWeaponSwitch", "", 0, "Check if actor weapon switching is prevented",
	1, 0, nullptr, Cmd_GetPreventWeaponSwitch_Execute, nullptr, nullptr, 0
};

void PreventWeaponSwitch_RegisterCommands(const void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->RegisterCommand(&kCommandInfo_SetPreventWeaponSwitch);
	nvseIntf->RegisterCommand(&kCommandInfo_GetPreventWeaponSwitch);
}
