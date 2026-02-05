//hides dismembered limbs in VATS targeting
//NOT hot-reloadable - requires game restart

#include "VATSLimbFix.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace VATSLimbFix
{
	constexpr uintptr_t kAddr_SetPartitionVisible = 0x5E4810;
	constexpr uintptr_t kAddr_VATSTargetRef = 0x11F21CC;
	constexpr uintptr_t kAddr_VATSTargetsList = 0x11DB150;
	constexpr uint8_t kExtraData_DismemberedLimbs = 0x5F;

	static uint8_t g_trampolineSetPartitionVisible[64];

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
		((SetPartitionVisible_t)(void*)g_trampolineSetPartitionVisible)(skinInstance, limbID, visible);
	}

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void WriteRelJump(uintptr_t src, uintptr_t dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)src = 0xE9;
		*(uint32_t*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init()
	{
		//prologue at 0x5E4810: push ebp (1) + mov ebp,esp (2) + sub esp,8 (3) = 6 bytes
		constexpr size_t kPrologueSize = 6;

		memcpy(g_trampolineSetPartitionVisible, (void*)kAddr_SetPartitionVisible, kPrologueSize);
		g_trampolineSetPartitionVisible[kPrologueSize] = 0xE9;
		*(uint32_t*)(g_trampolineSetPartitionVisible + kPrologueSize + 1) =
			(kAddr_SetPartitionVisible + kPrologueSize) - ((uintptr_t)g_trampolineSetPartitionVisible + kPrologueSize + 5);
		DWORD oldProtect;
		VirtualProtect(g_trampolineSetPartitionVisible, sizeof(g_trampolineSetPartitionVisible), PAGE_EXECUTE_READWRITE, &oldProtect);

		WriteRelJump(kAddr_SetPartitionVisible, (uintptr_t)SetPartitionVisible_Hook);
		PatchWrite8(kAddr_SetPartitionVisible + 5, 0x90); //nop leftover byte
		Log("VATSLimbFix installed");
	}
}

void VATSLimbFix_Init()
{
	VATSLimbFix::Init();
}
