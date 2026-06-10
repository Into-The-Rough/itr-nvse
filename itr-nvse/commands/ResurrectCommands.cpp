//ResurrectActorEx / ResurrectAll with inventory snapshot/restore

#include "ResurrectCommands.h"
#include "internal/CallTemplates.h"
#include "internal/BSSpinLock.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/GameProcess.h"
#include "nvse/GameExtraData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <vector>

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

static bool IsActorRef(TESObjectREFR* ref)
{
	if (!ref || !ref->baseForm) return false;
	return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
}

typedef void (__thiscall *_ActorResurrect)(Actor*, bool, bool, bool);
static const _ActorResurrect ActorResurrect = (_ActorResurrect)0x89F780;
static BSSpinLock* g_processListsActorLock = (BSSpinLock*)0x11F11A0;
typedef void* (__cdecl *_FormHeap_Allocate)(UInt32);
typedef void (__cdecl *_FormHeap_Free)(void*);
typedef void (__thiscall *_BaseExtraList_Copy)(void*, void*);
static const _FormHeap_Allocate s_formHeapAllocate = (_FormHeap_Allocate)0x401000;
static const _FormHeap_Free s_formHeapFree = (_FormHeap_Free)0x401030;
static const _BaseExtraList_Copy BaseExtraList_Copy = (_BaseExtraList_Copy)0x411EC0;

static void** g_modelLoader = (void**)0x11C3B3C;
typedef void (__thiscall *_ModelLoader_QueueReference)(void*, TESObjectREFR*, UInt32, bool);
static const _ModelLoader_QueueReference ModelLoader_QueueReference = (_ModelLoader_QueueReference)0x444850;
typedef NiNode* (__thiscall *_TESObjectREFR_Get3D)(TESObjectREFR*);
static const _TESObjectREFR_Get3D TESObjectREFR_Get3D = (_TESObjectREFR_Get3D)0x43FCD0;
static constexpr UInt32 kExtraDataListVtbl = 0x010143E8;
static constexpr UInt32 kExtraContainerChangesVtbl = 0x01015BB8;

enum ResurrectActorExFlags : UInt32
{
	kResurrectActorEx_ResetInventory = 1 << 0,
};

struct ResurrectActorExEntrySnapshot
{
	TESForm* type = nullptr;
	SInt32 countDelta = 0;
	std::vector<ExtraDataList*> extraLists;
};

struct ResurrectActorExInventorySnapshot
{
	float unk2 = 0.0f;
	float unk3 = 0.0f;
	UInt8 byte10 = 0;
	std::vector<ResurrectActorExEntrySnapshot> entries;
};

static ParamInfo kParams_ResurrectActorEx[1] = {
	{ "flags", kParamType_Integer, 1 },
};

DEFINE_COMMAND_PLUGIN(ResurrectActorEx, "Resurrect actor with flags: 1=reset inventory", 1, 1, kParams_ResurrectActorEx);
DEFINE_COMMAND_PLUGIN(ResurrectAll, "Resurrects all dead actors in high process", 0, 0, nullptr);

static void TESObjectREFR_Set3D(TESObjectREFR* ref, void* niNode, bool unloadArt)
{
	if (!ref) return;
	auto* vtbl = *(UInt32**)ref;
	if (!vtbl) return;
	auto fn = reinterpret_cast<void(__thiscall*)(TESObjectREFR*, void*, bool)>(vtbl[0x1CC / 4]);
	fn(ref, niNode, unloadArt);
}

static bool BaseExtraList_HasType(const BaseExtraList* list, UInt32 type)
{
	if (!list) return false;
	UInt32 index = (type >> 3);
	UInt8 bitMask = 1 << (type % 8);
	return (list->m_presenceBitfield[index] & bitMask) != 0;
}

static void BaseExtraList_MarkType(BaseExtraList* list, UInt32 type, bool cleared)
{
	if (!list) return;
	UInt32 index = (type >> 3);
	UInt8 bitMask = 1 << (type % 8);
	UInt8& flag = list->m_presenceBitfield[index];
	if (cleared)
		flag &= ~bitMask;
	else
		flag |= bitMask;
}

static BSExtraData* BaseExtraList_GetByTypeLocal(BaseExtraList* list, UInt32 type)
{
	if (!list || !BaseExtraList_HasType(list, type)) return nullptr;
	for (BSExtraData* traverse = list->m_data; traverse; traverse = traverse->next)
		if (traverse->type == type)
			return traverse;
	return nullptr;
}

static bool BaseExtraList_AddLocal(BaseExtraList* list, BSExtraData* toAdd)
{
	if (!list || !toAdd || BaseExtraList_HasType(list, toAdd->type))
		return false;

	toAdd->next = list->m_data;
	list->m_data = toAdd;
	BaseExtraList_MarkType(list, toAdd->type, false);
	return true;
}

template <class TList, class TItem>
static void AppendListItem(TList* list, TItem* item)
{
	if (!list || !item) return;

	using Node = typename TList::_Node;
	Node* head = list->Head();
	if (!head->item)
	{
		head->item = item;
		head->next = nullptr;
		return;
	}

	Node* node = head;
	while (node->next)
		node = node->next;

	Node* newNode = static_cast<Node*>(s_formHeapAllocate(sizeof(Node)));
	memset(newNode, 0, sizeof(Node));
	newNode->item = item;
	node->next = newNode;
}

static ExtraDataList* CreateEmptyExtraDataList()
{
	auto* list = static_cast<ExtraDataList*>(s_formHeapAllocate(sizeof(ExtraDataList)));
	memset(list, 0, sizeof(ExtraDataList));
	*(UInt32*)list = kExtraDataListVtbl;
	return list;
}

static ExtraContainerChanges* CreateEmptyExtraContainerChanges(TESObjectREFR* owner)
{
	auto* xChanges = static_cast<ExtraContainerChanges*>(s_formHeapAllocate(sizeof(ExtraContainerChanges)));
	memset(xChanges, 0, sizeof(ExtraContainerChanges));
	*(UInt32*)xChanges = kExtraContainerChangesVtbl;
	xChanges->type = kExtraData_ContainerChanges;

	xChanges->data = static_cast<ExtraContainerChanges::Data*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::Data)));
	memset(xChanges->data, 0, sizeof(ExtraContainerChanges::Data));
	xChanges->data->owner = owner;
	return xChanges;
}

static ExtraContainerChanges::EntryDataList* CreateEmptyEntryDataList()
{
	auto* list = static_cast<ExtraContainerChanges::EntryDataList*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::EntryDataList)));
	memset(list, 0, sizeof(ExtraContainerChanges::EntryDataList));
	return list;
}

static ExtraContainerChanges::ExtendDataList* CreateEmptyExtendDataList()
{
	auto* list = static_cast<ExtraContainerChanges::ExtendDataList*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::ExtendDataList)));
	memset(list, 0, sizeof(ExtraContainerChanges::ExtendDataList));
	return list;
}

static ExtraContainerChanges::EntryData* CreateEntryData(TESForm* form, SInt32 countDelta)
{
	auto* entry = static_cast<ExtraContainerChanges::EntryData*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::EntryData)));
	memset(entry, 0, sizeof(ExtraContainerChanges::EntryData));
	entry->type = form;
	entry->countDelta = countDelta;
	return entry;
}

static ExtraDataList* CloneExtraDataList(ExtraDataList* source)
{
	if (!source) return nullptr;
	auto* copy = CreateEmptyExtraDataList();
	BaseExtraList_Copy(copy, source);
	return copy;
}

static void DeleteExtraDataList(ExtraDataList* xData)
{
	if (!xData) return;
	//vtbl slot 0 is the scalar deleting dtor, flag 1 frees via FormHeap
	//~BaseExtraList runs RemoveAll(true) which destroys the whole BSExtraData
	//chain through each extra's virtual dtor (0x410380 -> 0x4103B0 -> 0x40F7B0)
	ThisCall<void>((*(UInt32**)xData)[0], xData, 1);
}

template <class TList, class TItem, class FreeItemFn>
static void FreeListOwned(TList* list, FreeItemFn&& freeItem)
{
	if (!list) return;

	using Node = typename TList::_Node;
	Node* node = list->Head();
	while (node)
	{
		Node* next = node->next;
		if (node->item)
			freeItem(node->item);
		if (node != list->Head())
			s_formHeapFree(node);
		node = next;
	}
	s_formHeapFree(list);
}

static void FreeEntryDataOwned(ExtraContainerChanges::EntryData* entry)
{
	if (!entry) return;
	if (entry->extendData)
	{
		FreeListOwned<ExtraContainerChanges::ExtendDataList, ExtraDataList>(
			entry->extendData,
			[](ExtraDataList* xData) { DeleteExtraDataList(xData); });
	}
	s_formHeapFree(entry);
}

static void FreeInventorySnapshot(ResurrectActorExInventorySnapshot& snapshot)
{
	for (auto& entry : snapshot.entries)
	{
		for (auto* xData : entry.extraLists)
			DeleteExtraDataList(xData);
		entry.extraLists.clear();
	}
	snapshot.entries.clear();
}

static ResurrectActorExInventorySnapshot CaptureInventorySnapshot(Actor* actor)
{
	ResurrectActorExInventorySnapshot snapshot;
	if (!actor) return snapshot;

	auto* xChanges = static_cast<ExtraContainerChanges*>(BaseExtraList_GetByTypeLocal(&actor->extraDataList, kExtraData_ContainerChanges));
	if (!xChanges || !xChanges->data || !xChanges->data->objList)
		return snapshot;

	snapshot.unk2 = xChanges->data->unk2;
	snapshot.unk3 = xChanges->data->unk3;
	snapshot.byte10 = xChanges->data->byte10;

	for (auto entryIter = xChanges->data->objList->Begin(); !entryIter.End(); ++entryIter)
	{
		auto* entry = entryIter.Get();
		if (!entry || !entry->type)
			continue;

		ResurrectActorExEntrySnapshot entrySnapshot;
		entrySnapshot.type = entry->type;
		entrySnapshot.countDelta = entry->countDelta;

		if (entry->extendData)
		{
			for (auto xDataIter = entry->extendData->Begin(); !xDataIter.End(); ++xDataIter)
			{
				if (auto* xData = xDataIter.Get())
					entrySnapshot.extraLists.push_back(CloneExtraDataList(xData));
			}
		}

		snapshot.entries.push_back(std::move(entrySnapshot));
	}

	return snapshot;
}

static void RestoreInventorySnapshot(Actor* actor, ResurrectActorExInventorySnapshot& snapshot)
{
	if (!actor) return;

	auto* xChanges = static_cast<ExtraContainerChanges*>(BaseExtraList_GetByTypeLocal(&actor->extraDataList, kExtraData_ContainerChanges));
	if (!xChanges)
	{
		xChanges = CreateEmptyExtraContainerChanges(actor);
		BaseExtraList_AddLocal(&actor->extraDataList, xChanges);
	}
	else if (!xChanges->data)
	{
		xChanges->data = static_cast<ExtraContainerChanges::Data*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::Data)));
		memset(xChanges->data, 0, sizeof(ExtraContainerChanges::Data));
	}

	//ActorResurrect(0x89F780) can cache pointers into this list, MoveToHigh(0x881D30)
	//stores a GetWornItem(0x4C8C10) copy whose extend list aliases a live entry's
	//ExtraDataList, clear the process weapon/ammo caches before freeing like
	//ResetInventory(0x574920) does
	ThisCall<void>(0x8ADC50, actor); //Actor::ClearProcessItems

	if (xChanges->data->objList)
		FreeListOwned<ExtraContainerChanges::EntryDataList, ExtraContainerChanges::EntryData>(
			xChanges->data->objList,
			[](ExtraContainerChanges::EntryData* entry) { FreeEntryDataOwned(entry); });

	xChanges->data->owner = actor;
	xChanges->data->unk2 = snapshot.unk2;
	xChanges->data->unk3 = snapshot.unk3;
	xChanges->data->byte10 = snapshot.byte10;
	xChanges->data->objList = snapshot.entries.empty() ? nullptr : CreateEmptyEntryDataList();

	for (auto& snapshotEntry : snapshot.entries)
	{
		auto* entry = CreateEntryData(snapshotEntry.type, snapshotEntry.countDelta);
		if (!snapshotEntry.extraLists.empty())
		{
			entry->extendData = CreateEmptyExtendDataList();
			for (auto* xData : snapshotEntry.extraLists)
				AppendListItem(entry->extendData, xData);
			snapshotEntry.extraLists.clear();
		}
		AppendListItem(xChanges->data->objList, entry);
	}

	snapshot.entries.clear();
}

bool Cmd_ResurrectAll_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 count = 0;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell) return true;

	auto ProcessCell = [&](TESObjectCELL* cell)
	{
		if (!cell) return;
		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* refr = iter.Get();
			if (!refr || refr == player) continue;

			UInt8 baseType = refr->baseForm ? refr->baseForm->typeID : 0;
			if (baseType != kFormType_Creature && baseType != kFormType_NPC) continue;

			Actor* actor = (Actor*)refr;
			if (actor->lifeState != 2) continue;

			//clear 3D first so resurrection doesn't reuse dismembered model
			TESObjectREFR_Set3D(refr, nullptr, true);

			ActorResurrect(actor, true, true, false);

			//queue model reload
			if (*g_modelLoader)
				ModelLoader_QueueReference(*g_modelLoader, refr, 1, false);

			count++;
		}
	};

	ProcessCell(player->parentCell);

	TESWorldSpace* world = player->parentCell->worldSpace;
	if (world && world->cellMap && !player->parentCell->IsInterior() && player->parentCell->coords)
	{
		SInt32 baseX = (SInt32)player->parentCell->coords->x;
		SInt32 baseY = (SInt32)player->parentCell->coords->y;

		for (SInt32 dx = -1; dx <= 1; dx++)
		{
			for (SInt32 dy = -1; dy <= 1; dy++)
			{
				if (dx == 0 && dy == 0) continue;
				UInt32 key = ((baseX + dx) << 16) | ((baseY + dy) & 0xFFFF);
				TESObjectCELL* cell = world->cellMap->Lookup(key);
				ProcessCell(cell);
			}
		}
	}

	*result = count;

	if (IsConsoleMode())
		Console_Print("ResurrectAll >> Resurrected %d actors", count);

	return true;
}

bool Cmd_ResurrectActorEx_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 flags = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &flags))
		return true;

	if (!IsActorRef(thisObj))
		return true;

	Actor* actor = static_cast<Actor*>(thisObj);
	const UInt32 normalizedFlags = flags & kResurrectActorEx_ResetInventory;
	const bool resetInventory = (normalizedFlags & kResurrectActorEx_ResetInventory) != 0;
	const bool has3D = TESObjectREFR_Get3D(thisObj) != nullptr;
	ResurrectActorExInventorySnapshot inventorySnapshot;

	if (!resetInventory)
	{
		inventorySnapshot = CaptureInventorySnapshot(actor);
	}

	if (has3D)
	{
		TESObjectREFR_Set3D(thisObj, nullptr, true);
	}

	{
		BSSpinLockScope actorLock(g_processListsActorLock);
		ActorResurrect(actor, true, has3D, false);
	}

	if (!resetInventory)
	{
		RestoreInventorySnapshot(actor, inventorySnapshot);
	}
	else
	{
		FreeInventorySnapshot(inventorySnapshot);
	}

	if (*g_modelLoader)
		ModelLoader_QueueReference(*g_modelLoader, thisObj, 1, false);

	*result = 1;
	return true;
}

namespace ResurrectCommands {

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ResurrectActorEx);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ResurrectAll);
}

}
