//fixes explosions from projectiles worn as pants
//NOT hot-reloadable - requires game restart

#include "ExplodingPantsFix.h"
#define ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/NVSEMinimal.h"
#undef ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"

#include "internal/globals.h"

namespace ExplodingPantsFix
{
	static Detours::CallDetour s_isAltTriggerDetour;
	typedef bool (__thiscall *_IsAltTrigger)(void*);

	bool __fastcall Hook_IsAltTrigger(void* projBase, void* projectileRef) {
		if (((_IsAltTrigger)s_isAltTriggerDetour.GetOverwrittenAddr())(projBase))
			return true;
		if (ProjectileRefHasFlag(projectileRef, kProjectileRefFlag_AltTrigger))
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
