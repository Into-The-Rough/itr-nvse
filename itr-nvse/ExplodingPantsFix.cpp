//fixes explosions from projectiles worn as pants

#include "ExplodingPantsFix.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace ExplodingPantsFix
{
	static const uint32_t kAddr_IsAltTriggerCall = 0x9C3204;
	static const uint32_t kAddr_IsAltTrigger = 0x975300;
	static uint32_t g_retAddr = 0x9C3209;

	static void* g_currentProjectile = nullptr;

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

	bool __cdecl Hook_IsAltTrigger(void* projBase) {
		if (((bool(__thiscall*)(void*))kAddr_IsAltTrigger)(projBase))
			return true;
		//flag 0x400 at offset 0xC8
		if (g_currentProjectile && (*(uint32_t*)((uint8_t*)g_currentProjectile + 0xC8) & 0x400))
			return true;
		return false;
	}

	__declspec(naked) void Hook_IsAltTrigger_Wrapper() {
		__asm {
			mov eax, [ebp-0A0h]
			mov g_currentProjectile, eax
			push ecx
			call Hook_IsAltTrigger
			add esp, 4
			jmp g_retAddr
		}
	}

	void Init() {
		WriteRelCall(kAddr_IsAltTriggerCall, (uint32_t)Hook_IsAltTrigger_Wrapper);
		Log("ExplodingPantsFix installed");
	}
}

void ExplodingPantsFix_Init()
{
	ExplodingPantsFix::Init();
}
