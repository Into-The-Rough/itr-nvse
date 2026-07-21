//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/Detours.h"
#include "internal/PickpocketHookLogic.h"

#include "internal/globals.h"

#include <type_traits>

namespace ReversePickpocketNoKarmaFix
{
	static constexpr UInt32 kAddr_TryPickpocket = 0x75E0B0;
	static constexpr UInt32 kCallSite_TryPickpocket1 = 0x75DBDA;
	static constexpr UInt32 kCallSite_TryPickpocket2 = 0x75DFA7;

	static bool g_enabled = false;
	static Detours::CallDetour s_tryPickpocketCall1;
	static Detours::CallDetour s_tryPickpocketCall2;

	using IsLiveGrenade_t = bool (__thiscall*)(void*, void*, void*, void*);

	static bool ShouldSkipKarma(void* menu, void* actor)
	{
		void* selectedEntry = GetContainerMenuSelection();
		void* player = *(void**)g_thePlayerPtr;
		if (!menu || !selectedEntry || !player || !actor)
			return false;

		bool isReverse = ContainerMenuGetCurrentItems(menu) == ContainerMenuGetLeftItems(menu);

		return PickpocketHookLogic::ShouldSkipKarma(menu, selectedEntry, player, actor, isReverse,
			[](void* menuArg, void* entryArg, void* playerArg, void* actorArg) {
				return ((IsLiveGrenade_t)0x75D510)(menuArg, entryArg, playerArg, actorArg);
			});
	}

	static bool CallOriginal(Detours::CallDetour& detour, void* menu, SInt32 incomingEdx, void* actor, SInt32 count)
	{
		auto original = reinterpret_cast<PickpocketHookLogic::TryPickpocket_t>(detour.GetOverwrittenAddr());
		return PickpocketHookLogic::ForwardTryPickpocket(original, menu, incomingEdx, actor, count);
	}

	static bool Hook_TryPickpocket(Detours::CallDetour& detour, void* menu, SInt32 incomingEdx, void* actor, SInt32 count)
	{
		if (g_enabled && ShouldSkipKarma(menu, actor))
			return true;

		return CallOriginal(detour, menu, incomingEdx, actor, count);
	}

	bool __fastcall Hook_TryPickpocket1(void* menu, SInt32 incomingEdx, void* actor, SInt32 count)
	{
		return Hook_TryPickpocket(s_tryPickpocketCall1, menu, incomingEdx, actor, count);
	}

	bool __fastcall Hook_TryPickpocket2(void* menu, SInt32 incomingEdx, void* actor, SInt32 count)
	{
		return Hook_TryPickpocket(s_tryPickpocketCall2, menu, incomingEdx, actor, count);
	}

	static_assert(std::is_same_v<decltype(&Hook_TryPickpocket1), PickpocketHookLogic::TryPickpocket_t>);
	static_assert(std::is_same_v<decltype(&Hook_TryPickpocket2), PickpocketHookLogic::TryPickpocket_t>);

	static bool IsVanillaCall(UInt32 callSite)
	{
		if (*reinterpret_cast<UInt8*>(callSite) != 0xE8)
		{
			Log("ReversePickpocketNoKarmaFix: hook site %08X is not a CALL; leaving it untouched", callSite);
			return false;
		}

		const UInt32 target = callSite + 5 + *reinterpret_cast<SInt32*>(callSite + 1);
		if (target != kAddr_TryPickpocket)
		{
			Log("ReversePickpocketNoKarmaFix: hook site %08X is owned by %08X; leaving it untouched", callSite, target);
			return false;
		}

		return true;
	}

	static bool InstallHooks()
	{
		if (s_tryPickpocketCall1.IsInstalled() || s_tryPickpocketCall2.IsInstalled())
		{
			if (s_tryPickpocketCall1.OwnsPatch() && s_tryPickpocketCall2.OwnsPatch())
				return true;

			Log("ReversePickpocketNoKarmaFix: another plugin owns a pickpocket hook; ITR fix remains disabled");
			return false;
		}

		//Validate both sites before changing either one. Foreign hooks take priority.
		if (!IsVanillaCall(kCallSite_TryPickpocket1) || !IsVanillaCall(kCallSite_TryPickpocket2))
			return false;

		if (!s_tryPickpocketCall1.WriteRelCallIfTarget(kCallSite_TryPickpocket1, kAddr_TryPickpocket, Hook_TryPickpocket1))
		{
			Log("ReversePickpocketNoKarmaFix: failed to install first pickpocket hook");
			return false;
		}

		if (!s_tryPickpocketCall2.WriteRelCallIfTarget(kCallSite_TryPickpocket2, kAddr_TryPickpocket, Hook_TryPickpocket2))
		{
			if (!s_tryPickpocketCall1.Remove())
				Log("ReversePickpocketNoKarmaFix: failed to roll back first pickpocket hook");
			else
				Log("ReversePickpocketNoKarmaFix: failed to install second pickpocket hook; first hook rolled back");
			return false;
		}

		return true;
	}

	static void UninstallHooks()
	{
		if (s_tryPickpocketCall2.IsInstalled() && !s_tryPickpocketCall2.Remove())
			Log("ReversePickpocketNoKarmaFix: second hook now belongs to another plugin; leaving it untouched");
		if (s_tryPickpocketCall1.IsInstalled() && !s_tryPickpocketCall1.Remove())
			Log("ReversePickpocketNoKarmaFix: first hook now belongs to another plugin; leaving it untouched");
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = false;
		if (!enabled)
		{
			UninstallHooks();
			return;
		}

		g_enabled = InstallHooks();
	}

	void Init(bool enabled)
	{
		SetEnabled(enabled);
	}
}
