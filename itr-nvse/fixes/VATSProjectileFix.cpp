//fixes projectile hit chance in VATS
//NOT hot-reloadable - requires game restart

#include "VATSProjectileFix.h"
#include "internal/NVSEMinimal.h"

extern void Log(const char* fmt, ...);

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

	constexpr UInt32 kAddr_VATSMenuUpdate = 0x7F3E00;
	constexpr UInt32 kAddr_UpdateHitChance = 0x7F1290;
	constexpr UInt32 kAddr_FindTarget = 0x7F3C90;
	constexpr UInt32 kAddr_HookSite = 0x7ED349;
	constexpr UInt32 kAddr_pTargetRef = 0x11F21CC;

	static UInt32 s_originalVATSMenuUpdate = 0;

	template <typename T_Ret = UInt32, typename ...Args>
	__forceinline T_Ret VATSThisCall(UInt32 _addr, const void* _this, Args ...args) {
		return ((T_Ret(__thiscall*)(const void*, Args...))_addr)(_this, std::forward<Args>(args)...);
	}

	static bool __fastcall VATSMenuUpdate_Hook(void* pThis)
	{
		bool result = VATSThisCall<bool>(s_originalVATSMenuUpdate, pThis);
		if (!result) return result;

		void** ppTargetRef = (void**)kAddr_pTargetRef;
		void* pTargetRef = *ppTargetRef;
		if (!pTargetRef) return result;

		SimpleListNode* pTargetEntry = VATSThisCall<SimpleListNode*>(kAddr_FindTarget, pThis, pTargetRef);
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
				VATSThisCall<double>(kAddr_UpdateHitChance, pThis, pIter);
			}
			pIter = pIter->GetNext();
		}

		return result;
	}

	void Init()
	{
		SInt32 currentDisp = *(SInt32*)(kAddr_HookSite + 1);
		s_originalVATSMenuUpdate = kAddr_HookSite + 5 + currentDisp;
		SafeWrite::Write32(kAddr_HookSite + 1, (UInt32)VATSMenuUpdate_Hook - kAddr_HookSite - 5);
		Log("VATSProjectileFix installed");
	}
}

void VATSProjectileFix_Init()
{
	VATSProjectileFix::Init();
}
