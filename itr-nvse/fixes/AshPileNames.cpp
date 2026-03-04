//shows original NPC name for ash piles
//hook is installed once; behavior is gated at runtime via Settings::bAshPileNames

#include "AshPileNames.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "internal/Detours.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "internal/globals.h"
namespace Settings { extern int bAshPileNames; }

namespace AshPileNames
{
	typedef const char* (__thiscall* GetBaseFullName_t)(TESObjectREFR* thisRef);
	static Detours::JumpDetour s_detour;
	static bool s_hookInstalled = false;

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

		BSExtraData* extraData = GetExtraDataByType(&ashPileRef->extraDataList, 0x89); //kExtraData_AshPileRef
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

	static bool IsGameLoading()
	{
		void* mgr = *(void**)0x11DE134;
		if (!mgr) return false;
		return *(bool*)((char*)mgr + 0x26);
	}

	static const char* __fastcall Hook_GetBaseFullName(TESObjectREFR* thisRef, void* edx)
	{
		if (!thisRef || !thisRef->baseForm)
			return "";

		if (!Settings::bAshPileNames || IsGameLoading())
			return s_detour.GetTrampoline<GetBaseFullName_t>()(thisRef);

		const char* actorName = GetActorNameFromAshPile(thisRef);
		if (actorName)
			return actorName;
		return s_detour.GetTrampoline<GetBaseFullName_t>()(thisRef);
	}

	//prologue: push ebp (1) + mov ebp,esp (2) + push ecx (1) + mov [ebp-4],ecx (3) = 7 bytes
	void Init()
	{
		if (s_hookInstalled)
			return;

		if (s_detour.WriteRelJump(0x55D520, Hook_GetBaseFullName, 7)) //TESObjectREFR::GetBaseFullName
		{
			s_hookInstalled = true;
			Log("AshPileNames installed");
		}
		else
		{
			Log("AshPileNames failed to install");
		}
	}
}

void AshPileNames_Init()
{
	AshPileNames::Init();
}
