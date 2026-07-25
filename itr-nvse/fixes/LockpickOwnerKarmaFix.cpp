//prevents lockpicking karma loss when the player already owns the locked ref

#include "LockpickOwnerKarmaFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/Detours.h"

namespace LockpickOwnerKarmaFix
{
	using GetOwner_t = void* (__thiscall*)(void*);

	static bool g_enabled = false;
	static Detours::CallDetour s_callDetour;

	static void* __fastcall Hook_GetOwnerForLockpickKarma(void* ref, void*)
	{
		auto getOwner = reinterpret_cast<GetOwner_t>(s_callDetour.GetOverwrittenAddr());
		void* owner = getOwner ? getOwner(ref) : Engine::TESObjectREFR_GetOwnerRawForm(ref);
		if (!g_enabled || !owner)
			return owner;

		void* player = *(void**)g_thePlayerPtr;
		if (player && Engine::TESObjectREFR_IsAnOwner(ref, player, true))
			return nullptr;

		return owner;
	}

	void Init(bool enabled)
	{
		if (!s_callDetour.WriteRelCall(0x78F74D, (UInt32)Hook_GetOwnerForLockpickKarma))
			return;
		g_enabled = enabled;
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
	}
}
