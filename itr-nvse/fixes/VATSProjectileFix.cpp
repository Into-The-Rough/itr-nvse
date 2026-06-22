//fixes projectile hit chance in VATS
//NOT hot-reloadable - requires game restart

#include "VATSProjectileFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"

namespace VATSProjectileFix
{

	struct SimpleListNode {
		void* item;
		SimpleListNode* next;
		bool IsEmpty() { return !item; }
		SimpleListNode* GetNext() { return next; }
	};

	struct VATSTarget {
		void* pReference;
		UInt32 eType;
		SimpleListNode bodyParts;
	};
	static_assert(offsetof(VATSTarget, bodyParts) == 0x08);

	struct VATSBodyPart {
		float screenPosX;
		float screenPosY;
		float relativePosX;
		float relativePosY;
		float relativePosZ;
		float posX;
		float posY;
		float posZ;
		UInt32 eBodyPart;
		float fPercentVisible;
		float fHitChance;
		bool bIsOnScreen;
		bool bChanceCalculated;
		bool bFirstTimeShown;
		bool bNeedsRecalc;
	};

	constexpr UInt32 kAddr_HookSite = 0x7ED349;

	static Detours::CallDetour s_vatsMenuUpdateCall;
	typedef bool(__thiscall* VATSMenuUpdate_t)(void*);

	static bool __fastcall VATSMenuUpdate_Hook(void* pThis)
	{
		auto original = reinterpret_cast<VATSMenuUpdate_t>(s_vatsMenuUpdateCall.GetOverwrittenAddr());
		bool result = original(pThis);
		if (!result) return result;

		void** ppTargetRef = (void**)0x11F21CC;
		void* pTargetRef = *ppTargetRef;
		if (!pTargetRef) return result;

		SimpleListNode* pTargetEntry = ThisCall<SimpleListNode*>(0x7F3C90, pThis, pTargetRef);
		if (!pTargetEntry || pTargetEntry->IsEmpty()) return result;

		VATSTarget* pTarget = (VATSTarget*)pTargetEntry->item;
		if (!pTarget) return result;

		//type 2 = projectile
		if (pTarget->eType != 2) return result;

		SimpleListNode* pIter = &pTarget->bodyParts;
		while (pIter && !pIter->IsEmpty()) {
			VATSBodyPart* pPart = (VATSBodyPart*)pIter->item;
			if (pPart) {
				pPart->fPercentVisible = 1.0f;
				pPart->bChanceCalculated = true;
				ThisCall<double>(0x7F1290, pThis, pIter);
			}
			pIter = pIter->GetNext();
		}

		return result;
	}

	void Init()
	{
		s_vatsMenuUpdateCall.WriteRelCall(kAddr_HookSite, VATSMenuUpdate_Hook);
	}
}

