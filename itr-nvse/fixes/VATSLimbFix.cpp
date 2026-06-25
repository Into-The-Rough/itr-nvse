//hides dismembered limbs in VATS targeting
//NOT hot-reloadable - requires game restart

#include "VATSLimbFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/NiLayout.h"
#include <cstring>

#include "internal/globals.h"

namespace VATSLimbFix
{
	static Detours::JumpDetour s_detour;

	BSExtraData* GetExtraDataByType(BaseExtraList* list, UInt8 type) {
		if (!list) return nullptr;
		return static_cast<BSExtraData*>(Engine::BaseExtraList_GetByType(list, type));
	}

	UInt16 GetDismemberMask(TESObjectREFR* ref) {
		if (!ref) return 0;
		BSExtraData* xDismember = GetExtraDataByType(&ref->extraDataList, kExtraData_DismemberedLimbs);
		return ExtraDismemberedLimbsGetMask(xDismember);
	}

	void* GetRefRootNode(TESObjectREFR* ref) {
		return TESObjectREFRGetNiNodeRaw(ref);
	}

	TESObjectREFR* FindOwnerRef(void* skinActorRoot) {
		if (!skinActorRoot) return nullptr;

		TESObjectREFR* targetRef = VATSGetCurrentTarget();
		if (targetRef && GetRefRootNode(targetRef) == skinActorRoot)
			return targetRef;

		auto* node = VATSTargetListGetHead(g_vatsTargetList);
		while (!VATSTargetNodeIsEmpty(node)) {
			auto* ref = node->item ? node->item->targetRef : nullptr;
			if (ref && GetRefRootNode(ref) == skinActorRoot)
				return ref;
			node = node->next;
		}
		return nullptr;
	}

	void __fastcall SetPartitionVisible_Hook(void* skinInstance, void* edx, uint16_t limbID, char visible) {
		if (visible) {
			void* actorRoot = NiSkinInstanceGetActorRoot(skinInstance);
			TESObjectREFR* owner = FindOwnerRef(actorRoot);
			if (owner && (GetDismemberMask(owner) & (1 << limbID)))
				return;
		}
		typedef void (__thiscall *SetPartitionVisible_t)(void*, uint16_t, char);
		s_detour.GetTrampoline<SetPartitionVisible_t>()(skinInstance, limbID, visible);
	}

	//prologue: push ebp (1) + mov ebp,esp (2) + sub esp,8 (3) = 6 bytes
	void Init()
	{
		s_detour.WriteRelJump(0x5E4810, SetPartitionVisible_Hook, 6);
	}
}

