//prevents karma loss when reverse pickpocketing non-grenades

#include "ReversePickpocketNoKarmaFix.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace ReversePickpocketNoKarmaFix
{
	static bool g_enabled = false;

	constexpr uint32_t kAddr_TryPickpocket = 0x75E0B0;
	constexpr uint32_t kAddr_IsLiveGrenade = 0x75D510;
	constexpr uint32_t kAddr_CallSite1 = 0x75DBDA;
	constexpr uint32_t kAddr_CallSite2 = 0x75DFA7;
	constexpr uint32_t kAddr_CurrentEntry = 0x11D93FC;
	constexpr uint32_t kAddr_Player = 0x11DEA3C;

	typedef bool (__thiscall *_IsLiveGrenade)(void*, void*, void*, void*);

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(uint32_t src, uint32_t dst) {
		PatchWrite8(src, 0xE8);
		PatchWrite32(src + 1, dst - src - 5);
	}

	bool __fastcall ShouldSkipKarma(void* menu, void* actor)
	{
		void* entry = *(void**)kAddr_CurrentEntry;
		void* player = *(void**)kAddr_Player;

		uint32_t currentItems = *(uint32_t*)((uint32_t)menu + 0xF8);
		bool isReverse = (currentItems == (uint32_t)menu + 0x98);

		if (isReverse && entry)
		{
			bool isLiveGrenade = ((_IsLiveGrenade)kAddr_IsLiveGrenade)(menu, entry, player, actor);
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
			ret 12
		}
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
		Log("ReversePickpocketNoKarmaFix %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		WriteRelCall(kAddr_CallSite1, (uint32_t)Hook_TryPickpocket);
		WriteRelCall(kAddr_CallSite2, (uint32_t)Hook_TryPickpocket);
		g_enabled = enabled;
		Log("ReversePickpocketNoKarmaFix initialized (enabled=%d)", enabled);
	}
}

void ReversePickpocketNoKarmaFix_Init(bool enabled)
{
	ReversePickpocketNoKarmaFix::Init(enabled);
}

void ReversePickpocketNoKarmaFix_SetEnabled(bool enabled)
{
	ReversePickpocketNoKarmaFix::SetEnabled(enabled);
}
