//SetItemModFlags / GetItemModFlags
//stamps the engine's weapon-mod flag (ExtraWeaponModFlags, xData 0x8D) onto an inventory item instance.
//the vanilla inventory list draws "+" for any item carrying it, type-agnostic (verified: InitFunc,
//UpdateInventoryMenu, sub_719EF0 all append "+" on sub_4BD820 with no type check), and the engine
//serialises 0x8D with the item - so the marker shows and persists, on armor too.
//ItemModFlagSafety neutralises the non-weapon-safe consumers (sub_4BD570, the stat-card mod block).
//
//called on the item's inventory reference, exactly like JIP's SetWeaponRefModFlags (minus its weapon gate):
//  rItemRef.SetItemModFlags 5
//resolved via xNVSE's InventoryReferenceGetForRefID (same NVSEDataInterface path ImperativeCommands
//uses for CreateEntry). falls back to a plain placed reference's own list if thisObj isn't an inv ref.

#include "ItemModFlagCommands.h"
#define FORMUTILS_USE_NVSE_TYPES
#include "internal/FormUtils.h"
#include "internal/EngineFunctions.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

using namespace FormUtils;

namespace
{
	constexpr UInt8  kXData_WeaponModFlags = 0x8D;
	constexpr UInt32 kVtbl_ExtraWeaponModFlags = 0x010159A4;
	constexpr UInt32 kVtbl_ExtraDataList = 0x010143E8;
	constexpr UInt32 kNVSEData_InventoryReferenceGetForRefID = 2;

	typedef void* (__cdecl* _FormHeapAlloc)(UInt32);
	typedef void  (__cdecl* _FormHeapFree)(void*);
	static const _FormHeapAlloc FormHeapAlloc = (_FormHeapAlloc)0x401000;
	static const _FormHeapFree  FormHeapFree = (_FormHeapFree)0x401030;

	//xNVSE inventory reference - layout per JIP InventoryRef / xNVSE InventoryReference
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

	//BaseExtraList helpers - manual presence-bitfield management, mirrors ImperativeCommands.cpp
	static bool ListHasType(BaseExtraList* list, UInt32 type)
	{
		if (!list) return false;
		return (list->m_presenceBitfield[type >> 3] & (1 << (type % 8))) != 0;
	}

	static void ListMarkType(BaseExtraList* list, UInt32 type, bool cleared)
	{
		if (!list) return;
		UInt8& flag = list->m_presenceBitfield[type >> 3];
		if (cleared) flag &= ~(1 << (type % 8));
		else         flag |= (1 << (type % 8));
	}

	static BSExtraData* ListGetByType(BaseExtraList* list, UInt32 type)
	{
		if (!ListHasType(list, type)) return nullptr;
		for (BSExtraData* iter = list->m_data; iter; iter = iter->next)
			if (iter->type == type) return iter;
		return nullptr;
	}

	static void ListAdd(BaseExtraList* list, BSExtraData* toAdd)
	{
		if (!list || !toAdd || ListHasType(list, toAdd->type)) return;
		toAdd->next = list->m_data;
		list->m_data = toAdd;
		ListMarkType(list, toAdd->type, false);
	}

	static void ListRemoveFree(BaseExtraList* list, BSExtraData* toRemove)
	{
		if (!list || !toRemove || !ListHasType(list, toRemove->type)) return;
		bool removed = false;
		if (list->m_data == toRemove) { list->m_data = toRemove->next; removed = true; }
		else for (BSExtraData* iter = list->m_data; iter; iter = iter->next)
			if (iter->next == toRemove) { iter->next = toRemove->next; removed = true; break; }
		if (!removed) return;
		ListMarkType(list, toRemove->type, true);
		FormHeapFree(toRemove);
	}

	static BSExtraData* CreateWeaponModFlags(UInt32 flags)
	{
		UInt8* x = (UInt8*)FormHeapAlloc(0x10);
		memset(x, 0, 0x10);
		*(UInt32*)x = kVtbl_ExtraWeaponModFlags;
		x[0x04] = kXData_WeaponModFlags;     //BSExtraData::type
		*(UInt32*)(x + 0x0C) = flags;        //installed-slot bitmask, read as a byte by sub_424940
		return (BSExtraData*)x;
	}

	static ExtraDataList* CreateExtraDataList()
	{
		ExtraDataList* list = (ExtraDataList*)FormHeapAlloc(sizeof(ExtraDataList));
		memset(list, 0, sizeof(ExtraDataList));
		*(UInt32*)list = kVtbl_ExtraDataList;
		return list;
	}

	static ExtraContainerChanges::ExtendDataList* CreateExtendDataList()
	{
		auto* list = (ExtraContainerChanges::ExtendDataList*)FormHeapAlloc(sizeof(ExtraContainerChanges::ExtendDataList));
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
		Node* newNode = (Node*)FormHeapAlloc(sizeof(Node));
		memset(newNode, 0, sizeof(Node));
		newNode->item = xData;
		node->next = newNode;
	}

	//the inv ref's own xData when present; otherwise create one on the entry (count-1 case = this instance)
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
		if (!entry->extendData) entry->extendData = CreateExtendDataList();
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

		//inventory reference -> its live instance xData; else a plain placed ref -> its own list
		BaseExtraList* xData;
		if (InvRef* invRef = g_invRefGetForID ? g_invRefGetForID(thisObj->refID) : nullptr) {
			xData = ResolveInstanceData(invRef, flags != 0);
			if (!xData) { *result = (flags == 0) ? 1 : 0; return true; }
		}
		else {
			xData = &thisObj->extraDataList;
		}

		BSExtraData* x = ListGetByType(xData, kXData_WeaponModFlags);
		if (flags) {
			if (x) *(UInt32*)((char*)x + 0x0C) = flags;
			else   ListAdd(xData, CreateWeaponModFlags(flags));
		}
		else if (x) {
			ListRemoveFree(xData, x);
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

		BSExtraData* x = ListGetByType(xData, kXData_WeaponModFlags);
		if (x) *result = *(UInt8*)((char*)x + 0x0C);
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
