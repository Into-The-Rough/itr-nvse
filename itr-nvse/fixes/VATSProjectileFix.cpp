//fixes projectile hit chance in VATS
//NOT hot-reloadable - requires game restart

#include "VATSProjectileFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"

namespace VATSProjectileFix
{
	constexpr UInt32 kAddr_HookSite = 0x7ED349;

	static Detours::CallDetour s_vatsMenuUpdateCall;
	typedef bool(__thiscall* VATSMenuUpdate_t)(void*);

	static bool __fastcall VATSMenuUpdate_Hook(void* pThis)
	{
		auto original = reinterpret_cast<VATSMenuUpdate_t>(s_vatsMenuUpdateCall.GetOverwrittenAddr());
		bool result = original(pThis);
		if (!result) return result;

		TESObjectREFR* pTargetRef = VATSGetCurrentTarget();
		if (!pTargetRef) return result;

		auto* pTargetEntry = ThisCall<BSSimpleListNodeView<VATSTargetView*>*>(0x7F3C90, pThis, pTargetRef);
		if (VATSTargetNodeIsEmpty(pTargetEntry)) return result;

		VATSTargetView* pTarget = pTargetEntry->item;
		if (!VATSTargetIsProjectile(pTarget)) return result;

		auto* pIter = &pTarget->bodyParts;
		while (!VATSBodyPartNodeIsEmpty(pIter)) {
			VATSBodyPartView* pPart = pIter->item;
			if (pPart) {
				VATSBodyPartForceVisible(pPart);
				ThisCall<double>(0x7F1290, pThis, pIter);
			}
			pIter = pIter->next;
		}

		return result;
	}

	void Init()
	{
		s_vatsMenuUpdateCall.WriteRelCall(kAddr_HookSite, VATSMenuUpdate_Hook);
	}
}
