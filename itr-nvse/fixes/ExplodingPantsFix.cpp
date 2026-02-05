//fixes explosions from projectiles worn as pants
//NOT hot-reloadable - requires game restart

#include "ExplodingPantsFix.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace ExplodingPantsFix
{
	static const uint32_t kAddr_IsAltTriggerCall = 0x9C3204;
	static const uint32_t kAddr_IsAltTrigger = 0x975300;
	static constexpr uint32_t g_retAddr = 0x9C3209;

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

	bool __fastcall Hook_IsAltTrigger(void* projBase, void* projectileRef) {
		if (!projBase) return false;
		if (((bool(__thiscall*)(void*))kAddr_IsAltTrigger)(projBase))
			return true;
		//flag 0x400 at offset 0xC8
		if (projectileRef && (*(uint32_t*)((uint8_t*)projectileRef + 0xC8) & 0x400))
			return true;
		return false;
	}

	__declspec(naked) void Hook_IsAltTrigger_Wrapper() {
		__asm {
			mov edx, [ebp-0A0h]
			call Hook_IsAltTrigger
			jmp g_retAddr
		}
	}

	void Init() {
		WriteRelJump(kAddr_IsAltTriggerCall, (uint32_t)Hook_IsAltTrigger_Wrapper);
		Log("ExplodingPantsFix installed");
	}
}

void ExplodingPantsFix_Init()
{
	ExplodingPantsFix::Init();
}
