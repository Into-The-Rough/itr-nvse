//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/CallTemplates.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace ReversePickpocketNoKarmaFix
{
	static bool g_enabled = false;
	static Detours::CallDetour s_tryPickpocketCall1;
	static Detours::CallDetour s_tryPickpocketCall2;
	static Detours::CallDetour s_rewardKarmaCall;
	typedef bool(__thiscall* TryPickpocket_t)(void*, void*, UInt32);
	typedef void(__thiscall* RewardKarma_t)(void*, int);

	typedef bool (__thiscall *_IsLiveGrenade)(void*, void*, void*, void*);

	//set only around our own call into sub_75E0B0, so its catch roll and crime
	//alarm still run while the inner RewardKarma leg is neutralised
	static thread_local bool g_suppressKarma = false;

	static const UInt32 kAddr_RewardKarmaCall = 0x75E230; //E8 -> PlayerCharacter::RewardKarma 0x94FD30, inside sub_75E0B0

	void __fastcall Hook_RewardKarma(void* player, void*, int amount)
	{
		if (g_suppressKarma)
			return;

		reinterpret_cast<RewardKarma_t>(s_rewardKarmaCall.GetOverwrittenAddr())(player, amount);
	}

	bool __fastcall ShouldSkipKarma(void* menu, void* actor)
	{
		void* entry = GetContainerMenuSelection();
		void* player = *(void**)g_thePlayerPtr;

		bool isReverse = ContainerMenuGetCurrentItems(menu) == ContainerMenuGetLeftItems(menu);

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
		return original(menu, actor, count);
	}

	static bool Hook_TryPickpocket(Detours::CallDetour& detour, void* menu, void* actor, UInt32 count)
	{
		//run the original either way so the catch roll and alarm fire, only gate the karma leg
		bool suppress = g_enabled && ShouldSkipKarma(menu, actor);
		if (suppress) g_suppressKarma = true;
		bool result = CallOriginal(detour, menu, actor, count);
		if (suppress) g_suppressKarma = false;
		return result;
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
		s_rewardKarmaCall.WriteRelCall(kAddr_RewardKarmaCall, Hook_RewardKarma);
		g_enabled = enabled;
	}
}
