//fixes explosions from projectiles worn as pants
//NOT hot-reloadable - requires game restart

#include "ExplodingPantsFix.h"
#include "internal/NVSEMinimal.h"

extern void Log(const char* fmt, ...);

namespace ExplodingPantsFix
{
	static const uint32_t kAddr_IsAltTriggerCall = 0x9C3204;
	static const uint32_t kAddr_IsAltTrigger = 0x975300;
	static constexpr uint32_t g_retAddr = 0x9C3209;

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
		SafeWrite::WriteRelJump(kAddr_IsAltTriggerCall, (UInt32)Hook_IsAltTrigger_Wrapper);
		Log("ExplodingPantsFix installed");
	}
}

void ExplodingPantsFix_Init()
{
	ExplodingPantsFix::Init();
}
