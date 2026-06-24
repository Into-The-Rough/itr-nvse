//hides dismembered limbs in VATS targeting
//NOT hot-reloadable - requires game restart

#include "VATSLimbFix.h"
#define ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/NVSEMinimal.h"
#undef ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "internal/NiLayout.h"
#include <cstring>

#include "internal/globals.h"

namespace VATSLimbFix
{
	static Detours::JumpDetour s_detour;

	struct BSExtraData {
		void** vtbl;
		uint8_t type;
		uint8_t pad[3];
		BSExtraData* next;
	};

	struct BaseExtraList {
		void** vtbl;
		BSExtraData* head;
		uint8_t presentBits[0x15];
		uint8_t pad1D[3];
	};

	struct ExtraDismemberedLimbs : BSExtraData {
		uint16_t dismemberedMask;
		uint8_t pad0E[2];
		int32_t unk10;
		void* weapon;
		int32_t unk18;
		bool wasEaten;
		uint8_t pad1D[3];
	};

	struct LimbFixREFR {
		void** vtbl;
		char pad04[0x40];
		BaseExtraList extraDataList;
	};

	struct VATSTargetLimb {
		LimbFixREFR* pReference;
		uint32_t eType;
	};

	struct VATSTargetNode {
		VATSTargetLimb* data;
		VATSTargetNode* next;
	};

	struct VATSTargetList {
		VATSTargetNode head;
	};

	BSExtraData* GetExtraDataByType(BaseExtraList* list, uint8_t type) {
		if (!list) return nullptr;
		return static_cast<BSExtraData*>(Engine::BaseExtraList_GetByType(list, type));
	}

	uint16_t GetDismemberMask(LimbFixREFR* ref) {
		if (!ref) return 0;
		ExtraDismemberedLimbs* xDismember = (ExtraDismemberedLimbs*)GetExtraDataByType(&ref->extraDataList, 0x5F);
		return xDismember ? xDismember->dismemberedMask : 0;
	}

	void* GetRefRootNode(LimbFixREFR* ref) {
		return TESObjectREFRGetNiNodeRaw(reinterpret_cast<TESObjectREFR*>(ref));
	}

	LimbFixREFR* FindOwnerRef(void* skinActorRoot) {
		if (!skinActorRoot) return nullptr;

		LimbFixREFR* targetRef = *(LimbFixREFR**)0x11F21CC;
		if (targetRef && GetRefRootNode(targetRef) == skinActorRoot)
			return targetRef;

		VATSTargetList* targetList = (VATSTargetList*)0x11DB150;
		if (targetList) {
			VATSTargetNode* node = &targetList->head;
			while (node && node->data) {
				if (node->data->pReference && GetRefRootNode(node->data->pReference) == skinActorRoot)
					return node->data->pReference;
				node = node->next;
			}
		}
		return nullptr;
	}

	void __fastcall SetPartitionVisible_Hook(void* skinInstance, void* edx, uint16_t limbID, char visible) {
		if (visible) {
			void* actorRoot = NiSkinInstanceGetActorRoot(skinInstance);
			LimbFixREFR* owner = FindOwnerRef(actorRoot);
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

