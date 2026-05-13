//prevents lockpicking karma loss when the player already owns the locked ref

#include "LockpickOwnerKarmaFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/Detours.h"

namespace LockpickOwnerKarmaFix
{
	static bool g_enabled = false;
	static Detours::CallDetour s_callDetour;

	static void* __fastcall Hook_GetOwnerForLockpickKarma(void* ref, void*)
	{
		void* owner = Engine::TESObjectREFR_GetOwnerRawForm(ref);
		if (!g_enabled || !owner)
			return owner;

		void* player = *(void**)0x11DEA3C;
		if (player && Engine::TESObjectREFR_IsAnOwner(ref, player, true))
			return nullptr;

		return owner;
	}

	void Init(bool enabled)
	{
		//LockPickMenu::Update successful-pick owner check
		if (!s_callDetour.WriteRelCall(0x78F74D, (UInt32)Hook_GetOwnerForLockpickKarma))
			return;
		g_enabled = enabled;
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
	}
}
