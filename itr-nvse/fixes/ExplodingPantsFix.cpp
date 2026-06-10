//fixes explosions from projectiles worn as pants
//NOT hot-reloadable - requires game restart

#include "ExplodingPantsFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace ExplodingPantsFix
{
	static Detours::CallDetour s_isAltTriggerDetour;
	typedef bool (__thiscall *_IsAltTrigger)(void*);

	bool __fastcall Hook_IsAltTrigger(void* projBase, void* projectileRef) {
		if (((_IsAltTrigger)s_isAltTriggerDetour.GetOverwrittenAddr())(projBase))
			return true;
		//flag 0x400 at offset 0xC8
		if (projectileRef && (*(uint32_t*)((uint8_t*)projectileRef + 0xC8) & 0x400))
			return true;
		return false;
	}

	__declspec(naked) void Hook_IsAltTrigger_Wrapper() {
		__asm {
			mov edx, [ebp-0A0h]     //projectileRef local -> fastcall arg2, ecx already has projBase
			call Hook_IsAltTrigger
			ret                     //entered via the detoured E8, callee takes no stack args
		}
	}

	void Init() {
		if (!s_isAltTriggerDetour.WriteRelCall(0x9C3204, (UInt32)Hook_IsAltTrigger_Wrapper))
			Log("ExplodingPantsFix: no CALL at 0x9C3204, disabled");
	}
}
