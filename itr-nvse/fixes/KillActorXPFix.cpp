//prevents XP reward when using "kill" command on already-dead actors

#include "KillActorXPFix.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace KillActorXPFix
{
	constexpr uint32_t kAddr_XPBlockStart = 0x5BE379;
	constexpr uint32_t kAddr_XPBlockEnd = 0x5BE3FA;
	constexpr uint32_t kAddr_ActorGetLevel = 0x87F9F0;
	constexpr uint32_t kAddr_ReturnAfterHook = 0x5BE381;
	constexpr uint32_t kOffset_Actor_LifeState = 0x108;

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

	void WriteRelJump(uint32_t src, uint32_t dst) {
		PatchWrite8(src, 0xE9);
		PatchWrite32(src + 1, dst - src - 5);
	}

	__declspec(naked) void Hook_XPBlockStart()
	{
		__asm
		{
			mov ecx, [ebp-0x10]
			mov eax, [ecx + 0x108] //lifeState: 0=alive,1=dying,2=dead
			cmp eax, 1
			je skip_xp
			cmp eax, 2
			je skip_xp

			mov eax, kAddr_ActorGetLevel
			call eax
			mov eax, kAddr_ReturnAfterHook
			jmp eax

		skip_xp:
			mov eax, kAddr_XPBlockEnd
			jmp eax
		}
	}

	void Init()
	{
		WriteRelJump(kAddr_XPBlockStart, (uint32_t)Hook_XPBlockStart);
		PatchWrite8(kAddr_XPBlockStart + 5, 0x90);
		PatchWrite8(kAddr_XPBlockStart + 6, 0x90);
		PatchWrite8(kAddr_XPBlockStart + 7, 0x90);
		Log("KillActorXPFix installed");
	}
}

void KillActorXPFix_Init()
{
	KillActorXPFix::Init();
}
