//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/GameGlobals.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace ReversePickpocketNoKarmaFix
{
	static bool g_enabled = false;
	static Detours::CallDetour s_tryPickpocketCall1;
	static Detours::CallDetour s_tryPickpocketCall2;
	typedef bool(__thiscall* TryPickpocket_t)(void*, void*, UInt32);

	typedef bool (__thiscall *_IsLiveGrenade)(void*, void*, void*, void*);

	bool __fastcall ShouldSkipKarma(void* menu, void* actor)
	{
		void* entry = *(void**)0x11D93FC;
		void* player = *(void**)g_thePlayerPtr;

		uint32_t currentItems = *(uint32_t*)((uint32_t)menu + 0xF8);
		bool isReverse = (currentItems == (uint32_t)menu + 0x98);

		if (isReverse && entry)
		{
			bool isLiveGrenade = ((_IsLiveGrenade)0x75D510)(menu, entry, player, actor);
			if (!isLiveGrenade)
				return true;
		}
		return false;
	}

	static bool CallOriginal(Detours::CallDetour& detour, void* menu, void* actor, UInt32 count)
	{
		auto original = reinterpret_cast<TryPickpocket_t>(detour.GetOverwrittenAddr());
		return original ? original(menu, actor, count) : false;
	}

	static bool Hook_TryPickpocket(Detours::CallDetour& detour, void* menu, void* actor, UInt32 count)
	{
		if (g_enabled && ShouldSkipKarma(menu, actor))
			return true;

		return CallOriginal(detour, menu, actor, count);
	}

	bool __fastcall Hook_TryPickpocket1(void* menu, void*, void* actor, UInt32 count)
	{
		return Hook_TryPickpocket(s_tryPickpocketCall1, menu, actor, count);
	}

	bool __fastcall Hook_TryPickpocket2(void* menu, void*, void* actor, UInt32 count)
	{
		return Hook_TryPickpocket(s_tryPickpocketCall2, menu, actor, count);
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		s_tryPickpocketCall1.WriteRelCall(0x75DBDA, Hook_TryPickpocket1);
		s_tryPickpocketCall2.WriteRelCall(0x75DFA7, Hook_TryPickpocket2);
		g_enabled = enabled;
	}
}

