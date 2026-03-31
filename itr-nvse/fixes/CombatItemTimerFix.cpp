//fixes NPCs using fCombatItemBuffTimer for stimpaks instead of fCombatItemRestoreTimer
//bethesda forgot an else clause in CombatState::ResetCombatItemTimer
//NOT hot-reloadable - requires game restart

#include "CombatItemTimerFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace CombatItemTimerFix
{
	//combat item types
	enum { COMBAT_ITEM_RESTORE = 0, COMBAT_ITEM_BUFF = 1, COMBAT_ITEM_COUNT = 2 };

	//game setting pointers
	struct Setting { void* vtbl; float f; const char* name; };
	static Setting** gs_fCombatItemRestoreTimer = (Setting**)0x11CFDD8;
	static Setting** gs_fCombatItemBuffTimer = (Setting**)0x11CF480;

	//AITimer at offset 0x1AC in CombatState
	struct AITimer
	{
		float targetTime;
		float elapsedTime;
		void Reset(float duration) {
			typedef void(__thiscall* Fn)(AITimer*, float);
			((Fn)0x8D7F40)(this, duration);
		}
	};

	//minimal CombatState for the fields we need
	struct CombatState
	{
		uint8_t pad[0x1A4];
		void* pCombatItems[2];      //0x1A4
		AITimer kCombatItemTimers[2]; //0x1AC
	};

	void __fastcall Hook_ResetCombatItemTimer(CombatState* combatState, void* edx, void* apItem)
	{
		for (int i = COMBAT_ITEM_RESTORE; i < COMBAT_ITEM_COUNT; ++i)
		{
			if (combatState->pCombatItems[i] == apItem)
			{
				float fTimer;
				if (i == COMBAT_ITEM_RESTORE)
					fTimer = (*gs_fCombatItemRestoreTimer)->f;
				else
					fTimer = (*gs_fCombatItemBuffTimer)->f;

				combatState->kCombatItemTimers[i].Reset(fTimer);
				return;
			}
		}
	}

	void Init()
	{
		SafeWrite::WriteRelCall(0x9DAB61, (UInt32)Hook_ResetCombatItemTimer);
	}
}

