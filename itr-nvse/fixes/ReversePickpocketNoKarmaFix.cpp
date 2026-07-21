//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/Detours.h"
#include "internal/PickpocketHookLogic.h"

#include "internal/globals.h"

#include <type_traits>

extern void Log(const char* fmt, ...);

namespace ReversePickpocketNoKarmaFix
{
	static constexpr UInt32 kAddr_TryPickpocket = 0x75E0B0;
	static constexpr UInt32 kCallSite_TryPickpocket1 = 0x75DBDA;
	static constexpr UInt32 kCallSite_TryPickpocket2 = 0x75DFA7;
	static constexpr UInt32 kCallSite_RewardKarma = 0x75E230; //E8 -> PlayerCharacter::RewardKarma inside sub_75E0B0
	static constexpr UInt32 kAddr_RewardKarma = 0x94FD30;

	static bool g_enabled = false;
	static Detours::CallDetour s_tryPickpocketCall1;
	static Detours::CallDetour s_tryPickpocketCall2;
	static Detours::CallDetour s_rewardKarmaCall;

	using IsLiveGrenade_t = bool (__thiscall*)(void*, void*, void*, void*);
	using RewardKarma_t = void (__thiscall*)(void*, int);

	//set only around our own call into sub_75E0B0, so its catch roll and crime
	//alarm still run while the inner RewardKarma leg is neutralised
	static thread_local bool g_suppressKarma = false;

	void __fastcall Hook_RewardKarma(void* player, void*, int amount)
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
		//run the original either way so the catch roll and alarm fire, only gate the karma leg
		bool suppress = g_enabled && ShouldSkipKarma(menu, actor);
		if (suppress) g_suppressKarma = true;
		bool result = CallOriginal(detour, menu, incomingEdx, actor, count);
		if (suppress) g_suppressKarma = false;
		return result;
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

	static bool IsVanillaCall(UInt32 callSite, UInt32 expectedTarget)
	{
		if (*reinterpret_cast<UInt8*>(callSite) != 0xE8)
		{
			Log("ReversePickpocketNoKarmaFix: hook site %08X is not a CALL; leaving it untouched", callSite);
			return false;
		}

		const UInt32 target = callSite + 5 + *reinterpret_cast<SInt32*>(callSite + 1);
		if (target != expectedTarget)
		{
			Log("ReversePickpocketNoKarmaFix: hook site %08X is owned by %08X; leaving it untouched", callSite, target);
			return false;
		}

		return true;
	}

	static bool InstallHooks()
	{
		if (s_tryPickpocketCall1.IsInstalled() || s_tryPickpocketCall2.IsInstalled() || s_rewardKarmaCall.IsInstalled())
		{
			if (s_tryPickpocketCall1.OwnsPatch() && s_tryPickpocketCall2.OwnsPatch() && s_rewardKarmaCall.OwnsPatch())
				return true;

			Log("ReversePickpocketNoKarmaFix: another plugin owns a pickpocket hook; ITR fix remains disabled");
			return false;
		}

		//validate all three sites before changing any, foreign hooks take priority
		if (!IsVanillaCall(kCallSite_TryPickpocket1, kAddr_TryPickpocket)
			|| !IsVanillaCall(kCallSite_TryPickpocket2, kAddr_TryPickpocket)
			|| !IsVanillaCall(kCallSite_RewardKarma, kAddr_RewardKarma))
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

		if (!s_rewardKarmaCall.WriteRelCallIfTarget(kCallSite_RewardKarma, kAddr_RewardKarma, Hook_RewardKarma))
		{
			bool r2 = s_tryPickpocketCall2.Remove();
			bool r1 = s_tryPickpocketCall1.Remove();
			if (r1 && r2)
				Log("ReversePickpocketNoKarmaFix: failed to install reward-karma hook; pickpocket hooks rolled back");
			else
				Log("ReversePickpocketNoKarmaFix: failed to install reward-karma hook and could not fully roll back");
			return false;
		}

		return true;
	}

	static void UninstallHooks()
	{
		if (s_rewardKarmaCall.IsInstalled() && !s_rewardKarmaCall.Remove())
			Log("ReversePickpocketNoKarmaFix: reward-karma hook now belongs to another plugin; leaving it untouched");
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
