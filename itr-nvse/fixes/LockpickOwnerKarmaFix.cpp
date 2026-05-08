//prevents lockpicking karma loss when the player already owns the locked ref

#include "LockpickOwnerKarmaFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/SafeWrite.h"

namespace LockpickOwnerKarmaFix
{
	static bool g_enabled = false;

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
		//LockPickMenu::Update successful-pick owner check.
		SafeWrite::WriteRelCall(0x78F74D, (UInt32)Hook_GetOwnerForLockpickKarma);
		g_enabled = enabled;
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
	}
}
