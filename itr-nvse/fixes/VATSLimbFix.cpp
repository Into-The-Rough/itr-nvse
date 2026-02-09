//hides dismembered limbs in VATS targeting
//NOT hot-reloadable - requires game restart

#include "VATSLimbFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include <cstring>

extern void Log(const char* fmt, ...);

namespace VATSLimbFix
{
	constexpr uintptr_t kAddr_SetPartitionVisible = 0x5E4810;
	constexpr uintptr_t kAddr_VATSTargetRef = 0x11F21CC;
	constexpr uintptr_t kAddr_VATSTargetsList = 0x11DB150;
	constexpr uint8_t kExtraData_DismemberedLimbs = 0x5F;

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
		if (!list || !list->head) return nullptr;
		uint8_t byteIndex = type >> 3;
		uint8_t bitMask = 1 << (type & 7);
		if (byteIndex < sizeof(list->presentBits) && !(list->presentBits[byteIndex] & bitMask))
			return nullptr;
		for (BSExtraData* data = list->head; data; data = data->next) {
			if (data->type == type)
				return data;
		}
		return nullptr;
	}

	uint16_t GetDismemberMask(LimbFixREFR* ref) {
		if (!ref) return 0;
		ExtraDismemberedLimbs* xDismember = (ExtraDismemberedLimbs*)GetExtraDataByType(&ref->extraDataList, kExtraData_DismemberedLimbs);
		return xDismember ? xDismember->dismemberedMask : 0;
	}

	bool IsLimbDismemberedOnAnyVATSTarget(uint16_t limbID) {
		LimbFixREFR** pTargetRef = (LimbFixREFR**)kAddr_VATSTargetRef;
		if (pTargetRef && *pTargetRef) {
			uint16_t mask = GetDismemberMask(*pTargetRef);
			if (mask & (1 << limbID))
				return true;
		}

		VATSTargetList* targetList = (VATSTargetList*)kAddr_VATSTargetsList;
		if (targetList) {
			VATSTargetNode* node = &targetList->head;
			while (node && node->data) {
				VATSTargetLimb* target = node->data;
				if (target && target->pReference) {
					uint16_t mask = GetDismemberMask(target->pReference);
					if (mask & (1 << limbID))
						return true;
				}
				node = node->next;
			}
		}
		return false;
	}

	void __fastcall SetPartitionVisible_Hook(void* skinInstance, void* edx, uint16_t limbID, char visible) {
		if (visible) {
			if (IsLimbDismemberedOnAnyVATSTarget(limbID))
				return;
		}
		typedef void (__thiscall *SetPartitionVisible_t)(void*, uint16_t, char);
		s_detour.GetTrampoline<SetPartitionVisible_t>()(skinInstance, limbID, visible);
	}

	//prologue: push ebp (1) + mov ebp,esp (2) + sub esp,8 (3) = 6 bytes
	void Init()
	{
		if (s_detour.WriteRelJump(kAddr_SetPartitionVisible, SetPartitionVisible_Hook, 6))
			Log("VATSLimbFix installed");
	}
}

void VATSLimbFix_Init()
{
	VATSLimbFix::Init();
}
