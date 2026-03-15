//disables combat weapon searching for specific actors
//hooks CombatState::CombatItemSearch, also triggers NPCAntidoteUse/NPCDoctorsBagUse checks

#include "NoWeaponSearch.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "internal/SafeWrite.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/CallTemplates.h"
#include "internal/settings.h"
#include "internal/globals.h"
#include "features/NPCAntidoteUse.h"
#include "features/NPCDoctorsBagUse.h"

extern const _ExtractArgs ExtractArgs;

static bool IsGameLoading()
{
	void* mgr = *(void**)0x11DE134;
	if (!mgr) return false;
	return *(bool*)((char*)mgr + 0x26);
}

namespace NoWeaponSearch
{
	static const int MAX_DISABLED = 64;
	static UInt32 g_disabled[MAX_DISABLED] = {0};
	static int g_count = 0;
	static CRITICAL_SECTION g_lock;
	static volatile LONG g_lockInit = 0;

	static void EnsureLockInit()
	{
		InitCriticalSectionOnce(&g_lockInit, &g_lock);
	}

	typedef bool (__thiscall *CombatItemSearch_t)(void* combatState);
	CombatItemSearch_t Original = (CombatItemSearch_t)0x99F6D0;

	static bool IsDisabled_Unlocked(UInt32 refID)
	{
		for (int i = 0; i < g_count; i++)
			if (g_disabled[i] == refID)
				return true;
		return false;
	}

	bool IsDisabled(UInt32 refID)
	{
		ScopedLock lock(&g_lock);
		return IsDisabled_Unlocked(refID);
	}

	bool __fastcall Hook(void* combatState, void* edx)
	{
		if (IsGameLoading())
			return Original(combatState);

		void* controller = *(void**)((char*)combatState + 0x1C4);
		if (!controller)
			return Original(combatState);
		Actor* actor = (Actor*)Engine::CombatController_GetPackageOwner(controller);
		if (!actor || !*(void**)((char*)actor + 0x68) || !*(void**)((char*)actor + 0x64))
			return Original(combatState);

		if (Settings::bNPCAntidoteUse)
			NPCAntidoteUse_Check(combatState);
		if (Settings::bNPCDoctorsBagUse)
			NPCDoctorsBagUse_Check(combatState);

		bool isDisabled = false;
		{
			ScopedLock lock(&g_lock);
			if (g_count > 0 && IsDisabled_Unlocked(actor->refID))
				isDisabled = true;
		}

		if (isDisabled)
			return false;

		return Original(combatState);
	}

	void Set(Actor* actor, bool disable)
	{
		if (!actor) return;
		UInt32 refID = actor->refID;

		ScopedLock lock(&g_lock);
		if (disable)
		{
			if (IsDisabled_Unlocked(refID)) return;
			if (g_count < MAX_DISABLED)
				g_disabled[g_count++] = refID;
		}
		else
		{
			for (int i = 0; i < g_count; i++)
			{
				if (g_disabled[i] == refID)
				{
					g_disabled[i] = g_disabled[--g_count];
					g_disabled[g_count] = 0;
					break;
				}
			}
		}
	}

	bool Get(Actor* actor)
	{
		if (!actor) return false;
		ScopedLock lock(&g_lock);
		return IsDisabled_Unlocked(actor->refID);
	}

	void Init()
	{
		EnsureLockInit();
		SafeWrite::WriteRelCall(0x998D50, (UInt32)Hook);
		Log("NoWeaponSearch: Hook installed at 0x998D50");
	}
}

inline bool IsActorRef(TESObjectREFR* ref) {
	if (!ref) return false;
	return ThisCall<bool>(*(UInt32*)(*(UInt32*)ref + 0x100), ref);
}

static ParamInfo kParams_SetNoWeaponSearch[1] = {
	{"disable", kParamType_Integer, 0}
};

bool Cmd_SetNoWeaponSearch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 disable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &disable))
		return true;

	if (IsActorRef(thisObj))
	{
		NoWeaponSearch::Set((Actor*)thisObj, disable != 0);
		*result = 1;
	}
	return true;
}

bool Cmd_GetNoWeaponSearch_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (IsActorRef(thisObj))
		*result = NoWeaponSearch::Get((Actor*)thisObj) ? 1 : 0;
	return true;
}

CommandInfo kCommandInfo_SetNoWeaponSearch = {
	"SetNoWeaponSearch", "", 0, "Disable weapon searching for actor",
	1, 1, kParams_SetNoWeaponSearch, Cmd_SetNoWeaponSearch_Execute, nullptr, nullptr, 0
};

CommandInfo kCommandInfo_GetNoWeaponSearch = {
	"GetNoWeaponSearch", "", 0, "Check if weapon searching is disabled",
	1, 0, nullptr, Cmd_GetNoWeaponSearch_Execute, nullptr, nullptr, 0
};

void NoWeaponSearch_Init()
{
	NoWeaponSearch::Init();
}

void NoWeaponSearch_RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetNoWeaponSearch);
	nvse->RegisterCommand(&kCommandInfo_GetNoWeaponSearch);
}
