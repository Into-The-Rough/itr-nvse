//extends VATS target highlighting beyond vanilla limit
//NOT hot-reloadable - requires game restart

#include "VATSExtender.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include <Windows.h>

extern void Log(const char* fmt, ...);

template <typename T_Ret = uint32_t, typename ...Args>
__forceinline T_Ret VE_ThisCall(uint32_t _addr, const void* _this, Args ...args) {
	return ((T_Ret(__thiscall*)(const void*, Args...))_addr)(_this, std::forward<Args>(args)...);
}

template <typename T_Ret = void, typename ...Args>
__forceinline T_Ret VE_CdeclCall(uint32_t _addr, Args ...args) {
	return ((T_Ret(__cdecl*)(Args...))_addr)(std::forward<Args>(args)...);
}

namespace VATSExtender
{
	static constexpr UInt32 kMaxOverflow = 256;
	UInt32 g_overflowRefIDs[kMaxOverflow];
	UInt32 g_overflowCount = 0;
	UInt32 g_lastVanillaCount = 0;

	UInt32 g_lastLocationID = 0;
	bool g_lastWasInterior = false;

	void** g_interfaceManager = (void**)0x11D8A80;

	typedef TESForm* (*_LookupFormByID)(UInt32 id);
	static const _LookupFormByID VE_LookupFormByID = (_LookupFormByID)0x004839C0;

	void WriteRelJump(UInt32 src, UInt32 dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE9;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void WriteRelCall(UInt32 src, UInt32 dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE8;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void* GetVATSHighlightData()
	{
		void* im = *g_interfaceManager;
		if (!im) return nullptr;
		return (void*)((UInt8*)im + 0x1DC);
	}

	void ClearOverflow()
	{
		g_overflowCount = 0;
	}

	void CheckForReset(void* vatsData)
	{
		UInt32 vanillaCount = *(UInt32*)((UInt8*)vatsData + 0x0C);
		if (vanillaCount == 0 && g_lastVanillaCount > 0)
		{
			ClearOverflow();
		}
		g_lastVanillaCount = vanillaCount;
	}

	bool IsTracked(UInt32 refID)
	{
		for (UInt32 i = 0; i < g_overflowCount; i++)
		{
			if (g_overflowRefIDs[i] == refID) return true;
		}
		return false;
	}

	void AddOverflowRef(TESObjectREFR* refr)
	{
		if (!refr) return;
		if (g_overflowCount >= kMaxOverflow) return;
		if (IsTracked(refr->refID)) return;
		if (!refr->GetNiNode()) return;

		g_overflowRefIDs[g_overflowCount++] = refr->refID;
	}

	void __cdecl CaptureOverflowRef(TESObjectREFR* refr)
	{
		if (!refr) return;

		TESObjectCELL* currentCell = refr->parentCell;
		if (currentCell)
		{
			bool isInterior = currentCell->IsInterior();
			UInt32 locationID = isInterior ? currentCell->refID :
				(currentCell->worldSpace ? currentCell->worldSpace->refID : 0);

			if ((isInterior != g_lastWasInterior || locationID != g_lastLocationID) && g_overflowCount > 0)
			{
				ClearOverflow();
			}

			g_lastWasInterior = isInterior;
			g_lastLocationID = locationID;
		}

		AddOverflowRef(refr);
	}

	__declspec(naked) void Hook_OnLimitReached()
	{
		__asm
		{
			pushad
			pushfd
			mov eax, [ebp+8]
			push eax
			call CaptureOverflowRef
			add esp, 4
			popfd
			popad
			push 0x800E45
			ret
		}
	}

	void __cdecl Hook_RenderScene(void* camera, void* accumulator)
	{
		void* vatsData = GetVATSHighlightData();
		if (vatsData) CheckForReset(vatsData);

		if (g_overflowCount > 0)
		{
			void* worldRoot = VE_CdeclCall<void*>(0x45C670);
			if (worldRoot)
			{
				void* cullingProcess = VE_ThisCall<void*>(0x8D80E0, worldRoot);
				if (cullingProcess)
				{
					int rendered = 0;
					for (UInt32 i = 0; i < g_overflowCount; i++)
					{
						TESForm* form = VE_LookupFormByID(g_overflowRefIDs[i]);
						if (!form || form->typeID != kFormType_Reference) continue;

						TESObjectREFR* refr = (TESObjectREFR*)form;
						NiNode* node = refr->GetNiNode();
						if (!node) continue;

						VE_CdeclCall(0xB6BEE0, camera, node, cullingProcess);
						rendered++;
					}

					if (rendered == 0) ClearOverflow();
				}
			}
		}

		VE_CdeclCall(0xB6C0D0, camera, accumulator);
	}

	void Init()
	{
		if (*(UInt8*)0x800DA4 == 0x68)
			WriteRelJump(0x800DA4, (UInt32)Hook_OnLimitReached);

		if (*(UInt8*)0x801993 == 0xE8)
			WriteRelCall(0x801993, (UInt32)Hook_RenderScene);

		Log("VATSExtender installed");
	}
}

void VATSExtender_Init()
{
	VATSExtender::Init();
}
