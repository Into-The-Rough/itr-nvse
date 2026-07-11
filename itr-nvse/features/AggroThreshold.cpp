//lets NPCs absorb a configurable number of player hits before turning hostile
//intercepts the faction-relation getter at the two hit-reaction call sites and
//reports player-neutral actors as ally/friend, so the game applies the
//iAllyHit*/iFriendHit* tolerance thresholds instead of aggroing on the first hit

#include "AggroThreshold.h"
#include <Windows.h>
#include <cstdint>

#include "internal/globals.h"
#include "internal/GameGlobals.h"
#include "internal/Detours.h"
#include "internal/settings.h"

namespace AggroThreshold
{
	static bool g_enabled = false;
	static bool g_installed = false;

	//both sites call GetFactionRelation (sub_8B87A0), verified E8 to 0x8B87A0
	static Detours::CallDetour s_healthDamageDetour; //0x898766 in HandleHealthDamage, nulls attacker on lethal ally hit
	static Detours::CallDetour s_attackedByDetour;   //0x898A54 in AttackedBy, picks friend/ally hit thresholds

	//Setting struct value is at +4 (sub_43D4D0 returns setting+4)
	static int* const g_iFriendHitNonCombatAllowed = (int*)0x11CD5B4;
	static int* const g_iFriendHitCombatAllowed = (int*)0x11CD208;
	static int* const g_iAllyHitNonCombatAllowed = (int*)0x11CD46C;
	static int* const g_iAllyHitCombatAllowed = (int*)0x11CD4B8;

	static bool g_gmstsCaptured = false;
	static int g_origFriendHitNonCombat, g_origFriendHitCombat, g_origAllyHitNonCombat, g_origAllyHitCombat;

	typedef uint32_t (__thiscall *_GetFactionRelation)(void* thisActor, void* targetActor, bool* isEnemy);

	static bool IsPlayerInCombat()
	{
		void* player = *g_thePlayerPtr;
		if (!player) return false;
		return *(bool*)((uint8_t*)player + 0xDF0); //actor bIsInCombat
	}

	static bool IsCreature(void* actor)
	{
		if (!actor) return false;
		uintptr_t* vtbl = *(uintptr_t**)actor;
		typedef bool (__thiscall *_IsCreature)(void*);
		return ((_IsCreature)vtbl[0x21C / 4])(actor); //vtable slot 135 IsCreature
	}

	static void ApplyThresholds()
	{
		if (!g_gmstsCaptured)
		{
			g_origFriendHitNonCombat = *g_iFriendHitNonCombatAllowed;
			g_origFriendHitCombat = *g_iFriendHitCombatAllowed;
			g_origAllyHitNonCombat = *g_iAllyHitNonCombatAllowed;
			g_origAllyHitCombat = *g_iAllyHitCombatAllowed;
			g_gmstsCaptured = true;
		}

		*g_iFriendHitNonCombatAllowed = Settings::iFriendHitNonCombatAllowed;
		*g_iFriendHitCombatAllowed = Settings::iFriendHitCombatAllowed;
		*g_iAllyHitNonCombatAllowed = Settings::iAllyHitNonCombatAllowed;
		*g_iAllyHitCombatAllowed = Settings::iAllyHitCombatAllowed;
	}

	//engine reads these GMSTs directly, so a runtime disable must put them back
	static void RestoreThresholds()
	{
		if (!g_gmstsCaptured) return;
		*g_iFriendHitNonCombatAllowed = g_origFriendHitNonCombat;
		*g_iFriendHitCombatAllowed = g_origFriendHitCombat;
		*g_iAllyHitNonCombatAllowed = g_origAllyHitNonCombat;
		*g_iAllyHitCombatAllowed = g_origAllyHitCombat;
	}

	static uint32_t SuppressRelation(void* thisActor, void* targetActor, bool* isEnemy, _GetFactionRelation orig, bool forceAlly)
	{
		uint32_t result = orig(thisActor, targetActor, isEnemy);
		if (!g_enabled || result != 0) return result;

		if (Settings::bOnlyCombat && !IsPlayerInCombat()) return result;
		if (Settings::bIgnoreCreatures && IsCreature(thisActor)) return result;

		if (Settings::bIgnoreFriendlyFire)
		{
			uint32_t* formFlags = (uint32_t*)((uint8_t*)thisActor + 0x8);
			*formFlags |= 0x100000; //permanently ignore player hits
		}

		//HandleHealthDamage only nulls attacker for ALLY, so always report ally there
		if (forceAlly) return 2;
		return (Settings::iSuppressionMode == 0) ? 3 : 2; //0=friend, 1=ally
	}

	static uint32_t __fastcall Hook_HandleHealthDamage(void* thisActor, void*, void* targetActor, bool* isEnemy)
	{
		return SuppressRelation(thisActor, targetActor, isEnemy,
			(_GetFactionRelation)s_healthDamageDetour.GetOverwrittenAddr(), true);
	}

	static uint32_t __fastcall Hook_AttackedBy(void* thisActor, void*, void* targetActor, bool* isEnemy)
	{
		return SuppressRelation(thisActor, targetActor, isEnemy,
			(_GetFactionRelation)s_attackedByDetour.GetOverwrittenAddr(), false);
	}

	//idempotent, standalone SuppressiveFireNVSE claims these call sites too, so
	//itr must not install while off-by-default or it silently breaks the standalone dll
	static void InstallHooks()
	{
		if (g_installed) return;

		bool a = s_healthDamageDetour.WriteRelCall(0x898766, (UInt32)Hook_HandleHealthDamage);
		bool b = s_attackedByDetour.WriteRelCall(0x898A54, (UInt32)Hook_AttackedBy);
		if (!a || !b)
			Log("AggroThreshold: hook install %d/%d (site already patched by another mod?)", a ? 1 : 0, b ? 1 : 0);
		g_installed = a && b;
	}

	void SetEnabled(bool enabled)
	{
		if (enabled && !g_installed)
			InstallHooks();

		g_enabled = enabled && g_installed;
		if (g_enabled)
			ApplyThresholds();
		else
			RestoreThresholds();
	}

	void Init(bool enabled)
	{
		if (enabled)
			InstallHooks();
		SetEnabled(enabled);
	}
}
