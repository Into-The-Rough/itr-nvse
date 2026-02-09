//shows original NPC name for ash piles
//NOT hot-reloadable - requires game restart

#include "AshPileNames.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "internal/Detours.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>

extern void Log(const char* fmt, ...);

namespace AshPileNames
{
	constexpr UInt32 kGetBaseFullNameAddr = 0x55D520;
	constexpr UInt32 kExtraData_AshPileRef = 0x89;

	typedef const char* (__thiscall* GetBaseFullName_t)(TESObjectREFR* thisRef);
	static Detours::JumpDetour s_detour;

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
		return s_detour.GetTrampoline<GetBaseFullName_t>()(thisRef);
	}

	//prologue: push ebp (1) + mov ebp,esp (2) + push ecx (1) + mov [ebp-4],ecx (3) = 7 bytes
	void Init()
	{
		if (s_detour.WriteRelJump(kGetBaseFullNameAddr, Hook_GetBaseFullName, 7))
			Log("AshPileNames installed");
	}
}

void AshPileNames_Init()
{
	AshPileNames::Init();
}
