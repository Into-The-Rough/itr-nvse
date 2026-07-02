#include "ContainerCommands.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

namespace
{
	static BSExtraData* BaseExtraListGetByType(BaseExtraList* list, UInt32 type)
	{
		if (!list)
			return nullptr;

		return static_cast<BSExtraData*>(Engine::BaseExtraList_GetByType(list, type));
	}

	static bool IsVisibleInventoryForm(TESForm* form, bool includeUnresolvedLeveled)
	{
		if (!form)
			return false;
		if (form->typeID == kFormType_LeveledItem)
			return includeUnresolvedLeveled;

		switch (form->typeID)
		{
			case kFormType_Armor:
			case kFormType_Clothing:
				return reinterpret_cast<TESObjectARMO*>(form)->bipedModel.IsPlayable();
			case kFormType_Weapon:
				return reinterpret_cast<TESObjectWEAP*>(form)->IsPlayable();
			case kFormType_Ammo:
				return reinterpret_cast<TESAmmo*>(form)->IsPlayable();
			default:
				return true;
		}
	}

	static ExtraContainerChanges::EntryDataList* GetContainerChangesList(TESObjectREFR* containerRef)
	{
		auto* changes = static_cast<ExtraContainerChanges*>(
			BaseExtraListGetByType(&containerRef->extraDataList, kExtraData_ContainerChanges));
		return changes ? changes->GetEntryDataList() : nullptr;
	}

	static ExtraContainerChanges::EntryData* FindEntryForItem(
		ExtraContainerChanges::EntryDataList* entryList,
		TESForm* item)
	{
		if (!entryList || !item)
			return nullptr;

		for (auto* node = entryList->Head(); node; node = node->Next())
		{
			auto* entry = node->Item();
			if (entry && entry->type == item)
				return entry;
		}

		return nullptr;
	}

	static bool EntryHasExtraType(ExtraContainerChanges::EntryData* entry, UInt32 extraType)
	{
		if (!entry || !entry->extendData)
			return false;

		for (auto* node = entry->extendData->Head(); node; node = node->Next())
		{
			auto* extraList = node->Item();
			if (extraList && BaseExtraListGetByType(extraList, extraType))
				return true;
		}

		return false;
	}

	static bool BaseContainerHasConcreteForm(TESContainer* container, TESForm* item)
	{
		if (!container || !item)
			return false;

		for (auto* node = container->formCountList.Head(); node; node = node->Next())
		{
			auto* formCount = node->Item();
			if (formCount && formCount->form == item && item->typeID != kFormType_LeveledItem)
				return true;
		}

		return false;
	}

	static int CountConcreteVisibleEntries(TESContainer* container, ExtraContainerChanges::EntryDataList* entryList)
	{
		int count = 0;
		const bool includeUnresolvedLeveled = !entryList;

		for (auto* node = container->formCountList.Head(); node; node = node->Next())
		{
			auto* formCount = node->Item();
			if (!formCount || !IsVisibleInventoryForm(formCount->form, includeUnresolvedLeveled))
				continue;

			int itemCount = formCount->count;
			if (auto* entry = FindEntryForItem(entryList, formCount->form))
			{
				if (EntryHasExtraType(entry, kExtraData_LeveledItem))
					itemCount = entry->countDelta;
				else
					itemCount += entry->countDelta;
			}

			if (itemCount > 0)
				++count;
		}

		if (!entryList)
			return count;

		for (auto* node = entryList->Head(); node; node = node->Next())
		{
			auto* entry = node->Item();
			if (!entry || entry->countDelta <= 0 || !IsVisibleInventoryForm(entry->type, false))
				continue;

			if (!BaseContainerHasConcreteForm(container, entry->type))
				++count;
		}

		return count;
	}

	static int GetVisibleInventoryEntryCount(TESObjectREFR* containerRef)
	{
		if (!containerRef)
			return 0;

		auto* container = ThisCall<TESContainer*>(0x55D310, containerRef);
		if (!container)
			return 0;

		auto* entryList = GetContainerChangesList(containerRef);
		return CountConcreteVisibleEntries(container, entryList);
	}

	DEFINE_COMMAND_PLUGIN(GetVisibleContainerInventoryCount,
		"returns a non-mutating visible container inventory entry count, treating unresolved leveled lists as potential loot",
		1, 0, nullptr);

	bool Cmd_GetVisibleContainerInventoryCount_Execute(COMMAND_ARGS)
	{
		*result = GetVisibleInventoryEntryCount(thisObj);
		return true;
	}
}

namespace ContainerCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_GetVisibleContainerInventoryCount);
	}
}
