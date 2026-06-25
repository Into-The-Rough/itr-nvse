//disables combat weapon searching for specific actors
//hooks CombatState::CombatItemSearch, also triggers NPCAntidoteUse/NPCDoctorsBagUse checks

#include "NoWeaponSearch.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/layout/Combat.h"
#include "internal/settings.h"
#include "internal/globals.h"
#include "features/NPCAntidoteUse.h"
#include "features/NPCDoctorsBagUse.h"

extern const _ExtractArgs ExtractArgs;

namespace NoWeaponSearch
{
	static const int MAX_DISABLED = 64;
	static UInt32 g_disabled[MAX_DISABLED] = {0};
	static int g_count = 0;
	static thread_local bool g_inCombatItemSearch = false;
	static CRITICAL_SECTION g_lock;
	static volatile LONG g_lockInit = 0;

	static void EnsureLockInit()
	{
		InitCriticalSectionOnce(&g_lockInit, &g_lock);
	}

	typedef bool (__thiscall *CombatItemSearch_t)(void* combatState);
	static Detours::CallDetour s_combatItemSearchCall;

	static bool CallOriginal(void* combatState)
	{
		auto original = reinterpret_cast<CombatItemSearch_t>(s_combatItemSearchCall.GetOverwrittenAddr());
		return original ? original(combatState) : true;
	}

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
		if (g_inCombatItemSearch)
			return CallOriginal(combatState);

		g_inCombatItemSearch = true;
		if (IsGameLoading())
		{
			g_inCombatItemSearch = false;
			return CallOriginal(combatState);
		}

		void* controller = CombatStateGetCombatController(combatState);
		if (!controller)
		{
			g_inCombatItemSearch = false;
			return CallOriginal(combatState);
		}
		Actor* actor = (Actor*)Engine::CombatController_GetPackageOwner(controller);
		if (!actor || !actor->baseProcess || !actor->renderState)
		{
			g_inCombatItemSearch = false;
			return CallOriginal(combatState);
		}

		if (Settings::bNPCAntidoteUse)
			NPCAntidoteUse::Check(combatState);
		if (Settings::bNPCDoctorsBagUse)
			NPCDoctorsBagUse::Check(combatState);

		bool isDisabled = false;
		{
			ScopedLock lock(&g_lock);
			if (g_count > 0 && IsDisabled_Unlocked(actor->refID))
				isDisabled = true;
		}

		if (isDisabled)
		{
			g_inCombatItemSearch = false;
			return false;
		}

		bool result = CallOriginal(combatState);
		g_inCombatItemSearch = false;
		return result;
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
		s_combatItemSearchCall.WriteRelCall(0x998D50, Hook);
	}
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

	if (Engine::TESObjectREFR_IsActor(thisObj))
	{
		NoWeaponSearch::Set((Actor*)thisObj, disable != 0);
		*result = 1;
	}
	return true;
}

bool Cmd_GetNoWeaponSearch_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (Engine::TESObjectREFR_IsActor(thisObj))
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

namespace NoWeaponSearch {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetNoWeaponSearch);
	nvse->RegisterCommand(&kCommandInfo_GetNoWeaponSearch);
}
}
