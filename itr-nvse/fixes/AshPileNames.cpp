//shows original NPC name for ash piles

#include "AshPileNames.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace AshPileNames
{
	constexpr UInt32 kGetBaseFullNameAddr = 0x55D520;
	constexpr UInt32 kReturnAddr = 0x55D527;
	constexpr UInt32 kExtraData_AshPileRef = 0x89;

	static UInt8 g_trampoline[32];

	typedef const char* (__thiscall* _TrampolineFunc)(TESObjectREFR* thisRef);
	static _TrampolineFunc CallTrampoline = nullptr;

	static BSExtraData* GetExtraDataByType(BaseExtraList* list, UInt32 type)
	{
		if (!list) return nullptr;
		UInt32 index = (type >> 3);
		UInt8 bitMask = 1 << (type % 8);
		if (!(list->m_presenceBitfield[index] & bitMask))
			return nullptr;
		for (BSExtraData* traverse = list->m_data; traverse; traverse = traverse->next)
			if (traverse->type == type)
				return traverse;
		return nullptr;
	}

	static const char* GetActorNameFromAshPile(TESObjectREFR* ashPileRef)
	{
		if (!ashPileRef) return nullptr;

		BSExtraData* extraData = GetExtraDataByType(&ashPileRef->extraDataList, kExtraData_AshPileRef);
		if (!extraData) return nullptr;

		TESObjectREFR* sourceRef = *(TESObjectREFR**)((UInt8*)extraData + 0x0C);
		if (!sourceRef || !sourceRef->baseForm) return nullptr;

		TESForm* baseForm = sourceRef->baseForm;
		UInt8 formType = baseForm->typeID;

		if (formType != kFormType_NPC && formType != kFormType_Creature)
			return nullptr;

		TESActorBase* actorBase = (TESActorBase*)baseForm;
		const char* name = actorBase->fullName.name.m_data;

		if (name && name[0])
			return name;

		return nullptr;
	}

	static const char* __fastcall Hook_GetBaseFullName(TESObjectREFR* thisRef, void* edx)
	{
		const char* actorName = GetActorNameFromAshPile(thisRef);
		if (actorName)
			return actorName;
		return CallTrampoline(thisRef);
	}

	void Init()
	{
		DWORD oldProtect;
		VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);

		memcpy(g_trampoline, (void*)kGetBaseFullNameAddr, 7);
		g_trampoline[7] = 0xE9;
		*(UInt32*)&g_trampoline[8] = kReturnAddr - ((UInt32)&g_trampoline[7] + 5);

		CallTrampoline = (_TrampolineFunc)(void*)g_trampoline;

		DWORD oldProtect2;
		VirtualProtect((void*)kGetBaseFullNameAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect2);
		*(UInt8*)kGetBaseFullNameAddr = 0xE9;
		*(UInt32*)(kGetBaseFullNameAddr + 1) = (UInt32)Hook_GetBaseFullName - kGetBaseFullNameAddr - 5;
		*(UInt8*)(kGetBaseFullNameAddr + 5) = 0x90;
		*(UInt8*)(kGetBaseFullNameAddr + 6) = 0x90;
		VirtualProtect((void*)kGetBaseFullNameAddr, 7, oldProtect2, &oldProtect2);

		Log("AshPileNames installed");
	}
}

void AshPileNames_Init()
{
	AshPileNames::Init();
}
