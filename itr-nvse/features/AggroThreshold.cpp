//overrides GetFactionRelation and iAllyHit*/iFriendHit* reads inside Actor::AttackedBy

#include "AggroThreshold.h"
#include <Windows.h>

#include "internal/globals.h"
#include "internal/GameGlobals.h"
#include "internal/Detours.h"
#include "internal/settings.h"

namespace AggroThreshold {
	static bool g_enabled = false;
	static bool g_installed = false;

	static Detours::CallDetour s_attackedByDetour; //0x898A54 GetFactionRelation, picks the friend/ally branch
	static Detours::CallDetour s_friendCombatDetour; //0x898A98 iFriendHitCombatAllowed
	static Detours::CallDetour s_friendNonCombatDetour; //0x898AA9 iFriendHitNonCombatAllowed
	static Detours::CallDetour s_allyCombatDetour; //0x898AC6 iAllyHitCombatAllowed
	static Detours::CallDetour s_allyNonCombatDetour; //0x898AD7 iAllyHitNonCombatAllowed

	//set by the relation hook, read by the four setting hooks later in the same AttackedBy call
	//main thread only
	static bool s_thresholdPending = false;

	using GetFactionRelation_t = UInt32(__thiscall*)(void* thisActor, void* targetActor, bool* isEnemy);
	using GetSettingValue_t = int*(__thiscall*)(void* setting);

	static bool IsPlayerInCombat()
	{
		void* player = *g_thePlayerPtr;
		if (!player) return false;
		return *(bool*)((UInt8*)player + 0xDF0); //actor bIsInCombat
	}

	static bool IsCreature(void* actor)
	{
		if (!actor) return false;
		UInt32* vtbl = *(UInt32**)actor;
		using IsCreature_t = bool(__thiscall*)(void*);
		return ((IsCreature_t)vtbl[0x21C / 4])(actor); //vtable slot 135 IsCreature
	}

	//assigned on every invocation, never merely set, because the 0x100000 form-flag check
	//right after this call can leave AttackedBy before any setting is read
	static UInt32 SuppressRelation(void* thisActor, void* targetActor, bool* isEnemy, GetFactionRelation_t orig)
	{
		UInt32 result = orig(thisActor, targetActor, isEnemy);
		s_thresholdPending = false;
		if (!g_enabled || result != 0) return result;

		if (Settings::bOnlyCombat && !IsPlayerInCombat()) return result;
		if (Settings::bIgnoreCreatures && IsCreature(thisActor)) return result;

		if (Settings::bIgnoreFriendlyFire) {
			UInt32* formFlags = (UInt32*)((UInt8*)thisActor + 0x8);
			*formFlags |= 0x100000; //permanently ignore player hits
		}

		s_thresholdPending = true;
		return (Settings::iSuppressionMode == 0) ? 3 : 2; //0=friend, 1=ally
	}

	static UInt32 __fastcall Hook_AttackedBy(void* thisActor, void* edx, void* targetActor, bool* isEnemy)
	{
		return SuppressRelation(thisActor, targetActor, isEnemy,
			(GetFactionRelation_t)s_attackedByDetour.GetOverwrittenAddr());
	}

	//engine dereferences the returned pointer immediately, so a pointer to the ini-backed
	//int is enough, a value of 1000 or more still means unlimited to the caller
	static int* ThresholdOverride(void* setting, const Detours::CallDetour& detour, int& value)
	{
		if (!s_thresholdPending)
			return ((GetSettingValue_t)detour.GetOverwrittenAddr())(setting);

		s_thresholdPending = false;
		return &value;
	}

	static int* __fastcall Hook_FriendHitCombat(void* setting, void* edx)
	{
		return ThresholdOverride(setting, s_friendCombatDetour, Settings::iFriendHitCombatAllowed);
	}

	static int* __fastcall Hook_FriendHitNonCombat(void* setting, void* edx)
	{
		return ThresholdOverride(setting, s_friendNonCombatDetour, Settings::iFriendHitNonCombatAllowed);
	}

	static int* __fastcall Hook_AllyHitCombat(void* setting, void* edx)
	{
		return ThresholdOverride(setting, s_allyCombatDetour, Settings::iAllyHitCombatAllowed);
	}

	static int* __fastcall Hook_AllyHitNonCombat(void* setting, void* edx)
	{
		return ThresholdOverride(setting, s_allyNonCombatDetour, Settings::iAllyHitNonCombatAllowed);
	}

	//idempotent, standalone SuppressiveFireNVSE claims the relation call site too, so
	//itr must not install while off-by-default or it silently breaks the standalone dll
	static void InstallHooks()
	{
		if (g_installed) return;

		int relation = s_attackedByDetour.WriteRelCall(0x898A54, (UInt32)Hook_AttackedBy) ? 1 : 0;
		int friendCombat = s_friendCombatDetour.WriteRelCall(0x898A98, (UInt32)Hook_FriendHitCombat) ? 1 : 0;
		int friendNonCombat = s_friendNonCombatDetour.WriteRelCall(0x898AA9, (UInt32)Hook_FriendHitNonCombat) ? 1 : 0;
		int allyCombat = s_allyCombatDetour.WriteRelCall(0x898AC6, (UInt32)Hook_AllyHitCombat) ? 1 : 0;
		int allyNonCombat = s_allyNonCombatDetour.WriteRelCall(0x898AD7, (UInt32)Hook_AllyHitNonCombat) ? 1 : 0;

		g_installed = relation && friendCombat && friendNonCombat && allyCombat && allyNonCombat;
		if (!g_installed)
			Log("AggroThreshold: hook install %d%d%d%d%d (site already patched by another mod?)",
				relation, friendCombat, friendNonCombat, allyCombat, allyNonCombat);
	}

	void SetEnabled(bool enabled)
	{
		if (enabled && !g_installed)
			InstallHooks();

		g_enabled = enabled && g_installed;
		if (!g_enabled)
			s_thresholdPending = false;
	}

	void Init(bool enabled)
	{
		if (enabled)
			InstallHooks();
		SetEnabled(enabled);
	}
}
