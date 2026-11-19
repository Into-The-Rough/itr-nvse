//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/Detours.h"
#include "internal/globals.h"

namespace ReversePickpocketNoKarmaFix {
	static bool g_enabled = false;
	static bool s_installed = false;
	static Detours::CallDetour s_rewardKarmaCall;

	//preserve full call-site machine shape (ECX, EDX, both stack args) across chained hooks
	using TryPickpocket_t = bool(__fastcall*)(void* menu, SInt32 incomingEdx, void* actor, SInt32 count);
	using IsLiveGrenade_t = bool(__thiscall*)(void*, void*, void*, void*);
	using RewardKarma_t = void(__thiscall*)(void*, int);

	static thread_local bool g_suppressKarma = false;

	void __fastcall Hook_RewardKarma(void* player, void* edx, int amount)
	{
		if (g_suppressKarma)
			return;

		reinterpret_cast<RewardKarma_t>(s_rewardKarmaCall.GetOverwrittenAddr())(player, amount);
	}

	static bool ShouldSkipKarma(void* menu, void* actor)
	{
		void* selectedEntry = GetContainerMenuSelection();
		void* player = *(void**)g_thePlayerPtr;
		if (!menu || !selectedEntry || !player || !actor)
			return false;

		if (ContainerMenuGetCurrentItems(menu) != ContainerMenuGetLeftItems(menu))
			return false;

		return !((IsLiveGrenade_t)0x75D510)(menu, selectedEntry, player, actor);
	}

	template<UInt32 kAddr>
	class TryPickpocketHook {
		static inline Detours::CallDetour s_detour;

		static bool __fastcall Hook(void* menu, SInt32 incomingEdx, void* actor, SInt32 count)
		{
			bool suppress = g_enabled && ShouldSkipKarma(menu, actor);
			if (suppress) g_suppressKarma = true;
			bool result = reinterpret_cast<TryPickpocket_t>(s_detour.GetOverwrittenAddr())(menu, incomingEdx, actor, count);
			if (suppress) g_suppressKarma = false;
			return result;
		}

	public:
		TryPickpocketHook() { s_detour.WriteRelCall(kAddr, Hook); }
	};

	static void InstallHooks()
	{
		if (s_installed)
			return;

		//ContainerMenu::TryPickpocket callers
		TryPickpocketHook<0x75DBDA>();
		TryPickpocketHook<0x75DFA7>();
		s_rewardKarmaCall.WriteRelCall(0x75E230, Hook_RewardKarma);
		s_installed = true;
	}

	void SetEnabled(bool enabled)
	{
		if (enabled && !s_installed)
			InstallHooks();
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		SetEnabled(enabled);
	}
}
