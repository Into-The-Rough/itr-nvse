//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace ReversePickpocketNoKarmaFix
{
	static bool g_enabled = false;

	constexpr UInt32 kAddr_TryPickpocket = 0x75E0B0; //used in naked asm

	typedef bool (__thiscall *_IsLiveGrenade)(void*, void*, void*, void*);

	bool __fastcall ShouldSkipKarma(void* menu, void* actor)
	{
		void* entry = *(void**)0x11D93FC;
		void* player = *(void**)0x11DEA3C;

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

	__declspec(naked) void Hook_TryPickpocket()
	{
		__asm
		{
			cmp g_enabled, 0
			je call_original

			push ecx
			mov edx, [esp+8]
			call ShouldSkipKarma
			pop ecx

			test al, al
			jnz skip

		call_original:
			jmp kAddr_TryPickpocket

			skip:
				mov al, 1
				ret 8
			}
		}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
		Log("ReversePickpocketNoKarmaFix %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		SafeWrite::WriteRelCall(0x75DBDA, (UInt32)Hook_TryPickpocket);
		SafeWrite::WriteRelCall(0x75DFA7, (UInt32)Hook_TryPickpocket);
		g_enabled = enabled;
		Log("ReversePickpocketNoKarmaFix initialized (enabled=%d)", enabled);
	}
}

