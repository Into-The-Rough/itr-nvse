//setitemmodflags and getitemmodflags
//sets ExtraWeaponModFlags 0x8D on an inventory item instance
//vanilla persists the extra data with the item
//itemmodflagsafety guards weapon-only display paths
//inventory refs resolve through xnvse InventoryReferenceGetForRefID
//placed refs use their own extra data list

#include "ItemModFlagCommands.h"
#define FORMUTILS_USE_NVSE_TYPES
#include "internal/FormUtils.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

using namespace FormUtils;

namespace
{
	constexpr UInt8  kXData_WeaponModFlags = 0x8D;
	constexpr UInt32 kNVSEData_InventoryReferenceGetForRefID = 2;

	//xnvse inventory reference layout
	struct InvRef
	{
		TESForm*                          type;         // 00
		ExtraContainerChanges::EntryData* entry;        // 04
		ExtraDataList*                    xData;        // 08 - the live instance extra-data list
		TESObjectREFR*                    containerRef; // 0C
		TESObjectREFR*                    tempRef;      // 10
	};
	typedef InvRef* (*InvRefGetForID_t)(UInt32 refID);
	static InvRefGetForID_t g_invRefGetForID = nullptr;

	static BSExtraData* GetExtraData(BaseExtraList* list, UInt32 type)
	{
		return list ? static_cast<BSExtraData*>(Engine::BaseExtraList_GetByType(list, type)) : nullptr;
	}

	static BSExtraData* CreateWeaponModFlags(UInt32 flags)
	{
		auto* x = static_cast<ExtraWeaponModFlagsView*>(Engine::FormHeap_Allocate(sizeof(ExtraWeaponModFlagsView)));
		if (!x) return nullptr;
		Engine::ExtraWeaponModFlags_Ctor(x, static_cast<UInt8>(flags));
		return reinterpret_cast<BSExtraData*>(x);
	}

	static ExtraDataList* CreateExtraDataList()
	{
		auto* list = static_cast<ExtraDataList*>(Engine::FormHeap_Allocate(sizeof(ExtraDataList)));
		if (!list) return nullptr;
		Engine::ExtraDataList_Ctor(list);
		return list;
	}

	static ExtraContainerChanges::ExtendDataList* CreateExtendDataList()
	{
		auto* list = static_cast<ExtraContainerChanges::ExtendDataList*>(
			Engine::FormHeap_Allocate(sizeof(ExtraContainerChanges::ExtendDataList)));
		if (!list) return nullptr;
		memset(list, 0, sizeof(ExtraContainerChanges::ExtendDataList));
		return list;
	}

	static void AppendExtendData(ExtraContainerChanges::ExtendDataList* list, ExtraDataList* xData)
	{
		if (!list || !xData) return;
		using Node = ExtraContainerChanges::ExtendDataList::_Node;
		Node* head = list->Head();
		if (!head->item) { head->item = xData; head->next = nullptr; return; }
		Node* node = head;
		while (node->next) node = node->next;
		Node* newNode = static_cast<Node*>(Engine::FormHeap_Allocate(sizeof(Node)));
		if (!newNode) return;
		memset(newNode, 0, sizeof(Node));
		newNode->item = xData;
		node->next = newNode;
	}

	//use inv ref xdata when present otherwise create entry xdata
	static ExtraDataList* ResolveInstanceData(InvRef* invRef, bool create)
	{
		if (invRef->xData) return invRef->xData;
		auto* entry = invRef->entry;
		if (!entry) return nullptr;
		if (entry->extendData) {
			auto it = entry->extendData->Begin();
			if (!it.End() && it.Get()) return it.Get();
		}
		if (!create) return nullptr;
		ExtraDataList* xData = CreateExtraDataList();
		if (!xData) return nullptr;
		if (!entry->extendData) entry->extendData = CreateExtendDataList();
		if (!entry->extendData) return nullptr;
		AppendExtendData(entry->extendData, xData);
		return xData;
	}

	static ParamInfo kParams_SetItemModFlags[1] = {
		{ "flags", kParamType_Integer, 1 },
	};

	DEFINE_COMMAND_PLUGIN(SetItemModFlags, "Sets the weapon-mod flag (the inventory '+') on this inventory reference's item. flags 0 removes it; default 1.", 1, 1, kParams_SetItemModFlags);
	DEFINE_COMMAND_PLUGIN(GetItemModFlags, "Returns the weapon-mod flag bits on this inventory reference's item.", 1, 0, nullptr);

	bool Cmd_SetItemModFlags_Execute(COMMAND_ARGS)
	{
		*result = 0;

		UInt32 flags = 1;
		if (!ExtractArgs(EXTRACT_ARGS, &flags) || !thisObj)
			return true;
		flags &= 7;

		//inventory ref uses instance xdata otherwise placed ref uses its own list
		BaseExtraList* xData;
		if (InvRef* invRef = g_invRefGetForID ? g_invRefGetForID(thisObj->refID) : nullptr) {
			xData = ResolveInstanceData(invRef, flags != 0);
			if (!xData) { *result = (flags == 0) ? 1 : 0; return true; }
		}
		else {
			xData = &thisObj->extraDataList;
		}

		if (flags) {
			BSExtraData* x = GetExtraData(xData, kXData_WeaponModFlags);
			if (x) reinterpret_cast<ExtraWeaponModFlagsView*>(x)->flags = static_cast<UInt8>(flags);
			else if (auto* created = CreateWeaponModFlags(flags))
				Engine::BaseExtraList_AddExtra(xData, created);
		}
		else {
			Engine::BaseExtraList_RemoveByType(xData, kXData_WeaponModFlags);
		}

		*result = 1;
		if (IsConsoleMode())
			Console_Print("SetItemModFlags >> flags=%d", flags);
		return true;
	}

	bool Cmd_GetItemModFlags_Execute(COMMAND_ARGS)
	{
		*result = 0;
		if (!thisObj)
			return true;

		BaseExtraList* xData;
		if (InvRef* invRef = g_invRefGetForID ? g_invRefGetForID(thisObj->refID) : nullptr)
			xData = ResolveInstanceData(invRef, false);
		else
			xData = &thisObj->extraDataList;
		if (!xData) return true;

		BSExtraData* x = GetExtraData(xData, kXData_WeaponModFlags);
		if (x) *result = reinterpret_cast<ExtraWeaponModFlagsView*>(x)->flags;
		return true;
	}
}

namespace ItemModFlagCommands {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetItemModFlags);
	nvse->RegisterCommand(&kCommandInfo_GetItemModFlags);

	if (auto* dataInterface = reinterpret_cast<NVSEDataInterface*>(nvse->QueryInterface(kInterface_Data)))
		g_invRefGetForID = reinterpret_cast<InvRefGetForID_t>(
			dataInterface->GetFunc(kNVSEData_InventoryReferenceGetForRefID));
}
}
