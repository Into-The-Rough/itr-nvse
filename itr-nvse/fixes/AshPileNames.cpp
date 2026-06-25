//ash pile names + null-safe GetBaseFullName wrapper
//hooks call-site to avoid JIP prologue conflict

#include "AshPileNames.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include <Windows.h>
#include <cstdint>

#include "internal/globals.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameSDK.h"
#include "internal/layout/ExtraData.h"
#include "internal/GameGlobals.h"
namespace Settings { extern int bAshPileNames; }

namespace AshPileNames
{
	static bool s_hookInstalled = false;
	static bool s_initFailed = false;
	static Detours::CallDetour s_getBaseFullNameCall;

	static const UInt32 kAddr_HookSite = 0x776F66; //call GetBaseFullName in SetHUDCrosshairStrings

	static BSExtraData* GetExtraDataByType(BaseExtraList* list, UInt32 type)
	{
		if (!list) return nullptr;
		return static_cast<BSExtraData*>(Engine::BaseExtraList_GetByType(list, type));
	}

	static const char* GetActorNameFromAshPile(TESObjectREFR* ashPileRef)
	{
		if (!ashPileRef) return nullptr;

		BSExtraData* extraData = GetExtraDataByType(&ashPileRef->extraDataList, 0x89); //kExtraData_AshPileRef
		if (!extraData) return nullptr;

		TESObjectREFR* sourceRef = ExtraAshPileRefGetSourceRef(extraData);
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

		auto original = reinterpret_cast<GetBaseFullName_t>(s_getBaseFullNameCall.GetOverwrittenAddr());
		return original(thisRef);
	}

	void Init()
	{
		if (s_hookInstalled || !Settings::bAshPileNames)
			return;

		if (!s_getBaseFullNameCall.WriteRelCall(kAddr_HookSite, Hook_GetBaseFullName))
		{
			Log("AshPileNames failed: expected call opcode at 0x%08X", kAddr_HookSite);
			s_initFailed = true;
			return;
		}

		s_hookInstalled = true;
	}

	void Update()
	{
		if (!s_hookInstalled && !s_initFailed && Settings::bAshPileNames)
			Init();
	}
}
