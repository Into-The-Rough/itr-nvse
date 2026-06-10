//allows NPCs to hurt each other with non-explosive weapons

#include "FriendlyFire.h"
#include <Windows.h>

#include "internal/globals.h"
#include "internal/Detours.h"

namespace FriendlyFire
{
	static bool g_enabled = false;
	static bool g_installed = false;
	static Detours::CallDetour s_inCombatWithDetour;     //0x9C314E in Projectile::9C30D0
	static Detours::CallDetour s_isCombatTargetDetour;   //0x899D50 in Actor::899CB0

	typedef bool (__thiscall *_ActorBoolFn)(void*, void*);

	static bool __fastcall Hook_IsInCombatWithActor(void* thisActor, void*, void* otherActor)
	{
		if (g_enabled) return true;
		return ((_ActorBoolFn)s_inCombatWithDetour.GetOverwrittenAddr())(thisActor, otherActor);
	}

	static bool __fastcall Hook_IsActoraCombatTarget(void* controller, void*, void* actor)
	{
		if (g_enabled) return true;
		return ((_ActorBoolFn)s_isCombatTargetDetour.GetOverwrittenAddr())(controller, actor);
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled && g_installed;
	}

	void Init(bool enabled)
	{
		bool a = s_inCombatWithDetour.WriteRelCall(0x9C314E, (UInt32)Hook_IsInCombatWithActor);
		bool b = s_isCombatTargetDetour.WriteRelCall(0x899D50, (UInt32)Hook_IsActoraCombatTarget);
		if (!a || !b)
			Log("FriendlyFire: hook install %d/%d (site already patched by another mod?)", a ? 1 : 0, b ? 1 : 0);
		g_installed = a && b;
		g_enabled = enabled && g_installed;
	}
}
