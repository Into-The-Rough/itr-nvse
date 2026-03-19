//ash pile names + null-safe GetBaseFullName wrapper
//hooks call-site to avoid JIP prologue conflict

#include "AshPileNames.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include <Windows.h>
#include <cstdint>

#include "internal/globals.h"
#include "internal/SafeWrite.h"
namespace Settings { extern int bAshPileNames; }

namespace AshPileNames
{
	static bool s_hookInstalled = false;
	static UInt32 s_originalCallTarget = 0;

	static const UInt32 kAddr_HookSite = 0x776F66; //call GetBaseFullName in SetHUDCrosshairStrings

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

	static UInt32 ReadCallTarget(UInt32 addr)
	{
		UInt8* p = (UInt8*)addr;
		if (p[0] != 0xE8) return 0;
		return addr + 5 + *(SInt32*)(addr + 1);
	}

	typedef const char* (__thiscall* GetBaseFullName_t)(TESObjectREFR* apRef);

	static const char* __fastcall Hook_GetBaseFullName(TESObjectREFR* thisRef, void* edx)
	{
		if (!thisRef || !thisRef->baseForm)
			return "";

		if (Settings::bAshPileNames && !IsGameLoading())
		{
			const char* actorName = GetActorNameFromAshPile(thisRef);
			if (actorName)
				return actorName;
		}

		if (!s_originalCallTarget)
			return "";
		return ((GetBaseFullName_t)s_originalCallTarget)(thisRef);
	}

	void Init()
	{
		if (s_hookInstalled || !Settings::bAshPileNames)
			return;

		s_originalCallTarget = ReadCallTarget(kAddr_HookSite);
		if (!s_originalCallTarget)
		{
			Log("AshPileNames failed: expected call opcode at 0x%08X", kAddr_HookSite);
			return;
		}

		SafeWrite::WriteRelCall(kAddr_HookSite, (UInt32)Hook_GetBaseFullName);
		s_hookInstalled = true;
		Log("AshPileNames installed at call-site 0x%08X (original=0x%08X)", kAddr_HookSite, s_originalCallTarget);
	}

	void Update()
	{
		if (!s_hookInstalled && Settings::bAshPileNames)
			Init();
	}
}

