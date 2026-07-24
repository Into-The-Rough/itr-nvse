//ResurrectActorEx / ResurrectAll with inventory snapshot/restore

#include "ResurrectCommands.h"
#include "internal/CallTemplates.h"
#include "internal/BSSpinLock.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
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

typedef NiNode* (__thiscall *_TESObjectREFR_Get3D)(TESObjectREFR*);
static const _TESObjectREFR_Get3D TESObjectREFR_Get3D = (_TESObjectREFR_Get3D)0x43FCD0;

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

template <class TList, class TItem>
static bool AppendListItem(TList* list, TItem* item)
{
	if (!list || !item) return false;

	using Node = typename TList::_Node;
	Node* head = list->Head();
	if (!head->item)
	{
		//keep head->next intact, a null head item can front a populated tail
		head->item = item;
		return true;
	}

	Node* node = head;
	while (node->next)
		node = node->next;

	Node* newNode = static_cast<Node*>(Engine::FormHeap_Allocate(sizeof(Node)));
	if (!newNode) return false;
	memset(newNode, 0, sizeof(Node));
	newNode->item = item;
	node->next = newNode;
	return true;
}

static ExtraDataList* CreateEmptyExtraDataList()
{
	auto* list = static_cast<ExtraDataList*>(Engine::FormHeap_Allocate(sizeof(ExtraDataList)));
	if (!list) return nullptr;
	Engine::ExtraDataList_Ctor(list);
	return list;
}

static ExtraContainerChanges::Data* CreateEmptyContainerChangesData(TESObjectREFR* owner)
{
	auto* data = static_cast<ExtraContainerChanges::Data*>(Engine::FormHeap_Allocate(sizeof(ExtraContainerChanges::Data)));
	if (!data) return nullptr;
	memset(data, 0, sizeof(ExtraContainerChanges::Data));
	data->owner = owner;
	return data;
}

static ExtraContainerChanges::EntryDataList* CreateEmptyEntryDataList()
{
	auto* list = static_cast<ExtraContainerChanges::EntryDataList*>(
		Engine::FormHeap_Allocate(sizeof(ExtraContainerChanges::EntryDataList)));
	if (!list) return nullptr;
	memset(list, 0, sizeof(ExtraContainerChanges::EntryDataList));
	return list;
}

static ExtraContainerChanges::ExtendDataList* CreateEmptyExtendDataList()
{
	auto* list = static_cast<ExtraContainerChanges::ExtendDataList*>(
		Engine::FormHeap_Allocate(sizeof(ExtraContainerChanges::ExtendDataList)));
	if (!list) return nullptr;
	memset(list, 0, sizeof(ExtraContainerChanges::ExtendDataList));
	return list;
}

static ExtraContainerChanges::EntryData* CreateEntryData(TESForm* form, SInt32 countDelta)
{
	auto* entry = static_cast<ExtraContainerChanges::EntryData*>(
		Engine::FormHeap_Allocate(sizeof(ExtraContainerChanges::EntryData)));
	if (!entry) return nullptr;
	memset(entry, 0, sizeof(ExtraContainerChanges::EntryData));
	entry->type = form;
	entry->countDelta = countDelta;
	return entry;
}

static ExtraDataList* CloneExtraDataList(ExtraDataList* source)
{
	if (!source) return nullptr;
	auto* copy = CreateEmptyExtraDataList();
	if (!copy) return nullptr;
	Engine::ExtraDataList_Copy(copy, source);
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
			Engine::FormHeap_Free(node);
		node = next;
	}
	Engine::FormHeap_Free(list);
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
	Engine::FormHeap_Free(entry);
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

	auto* xChanges = static_cast<ExtraContainerChanges*>(
		Engine::BaseExtraList_GetByType(&actor->extraDataList, kExtraData_ContainerChanges));
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

	auto* xChanges = static_cast<ExtraContainerChanges*>(
		Engine::BaseExtraList_GetByType(&actor->extraDataList, kExtraData_ContainerChanges));
	if (!xChanges || !xChanges->data)
	{
		auto* data = CreateEmptyContainerChangesData(actor);
		if (!data)
		{
			FreeInventorySnapshot(snapshot);
			return;
		}
		Engine::ExtraDataList_AppendExtraContainerChangeData(&actor->extraDataList, data);
		xChanges = static_cast<ExtraContainerChanges*>(
			Engine::BaseExtraList_GetByType(&actor->extraDataList, kExtraData_ContainerChanges));
		if (!xChanges || !xChanges->data)
		{
			Engine::FormHeap_Free(data);
			FreeInventorySnapshot(snapshot);
			return;
		}
	}

	//ActorResurrect(0x89F780) can cache pointers into this list, MoveToHigh(0x881D30)
	//stores a GetWornItem(0x4C8C10) copy whose extend list aliases a live entry's
	//ExtraDataList, clear the process weapon/ammo caches before freeing like
	//ResetInventory(0x574920) does
	Engine::Actor_ClearProcessItems(actor);

	if (xChanges->data->objList)
		FreeListOwned<ExtraContainerChanges::EntryDataList, ExtraContainerChanges::EntryData>(
			xChanges->data->objList,
			[](ExtraContainerChanges::EntryData* entry) { FreeEntryDataOwned(entry); });

	xChanges->data->owner = actor;
	xChanges->data->unk2 = snapshot.unk2;
	xChanges->data->unk3 = snapshot.unk3;
	xChanges->data->byte10 = snapshot.byte10;
	xChanges->data->objList = snapshot.entries.empty() ? nullptr : CreateEmptyEntryDataList();
	if (!snapshot.entries.empty() && !xChanges->data->objList)
	{
		FreeInventorySnapshot(snapshot);
		return;
	}

	for (auto& snapshotEntry : snapshot.entries)
	{
		auto* entry = CreateEntryData(snapshotEntry.type, snapshotEntry.countDelta);
		if (!entry)
		{
			for (auto* xData : snapshotEntry.extraLists)
				DeleteExtraDataList(xData);
			snapshotEntry.extraLists.clear();
			continue;
		}
		if (!snapshotEntry.extraLists.empty())
		{
			entry->extendData = CreateEmptyExtendDataList();
			if (entry->extendData)
			{
				for (auto* xData : snapshotEntry.extraLists)
				{
					if (!AppendListItem(entry->extendData, xData))
						DeleteExtraDataList(xData);
				}
			}
			else
			{
				for (auto* xData : snapshotEntry.extraLists)
					DeleteExtraDataList(xData);
			}
			snapshotEntry.extraLists.clear();
		}
		if (!AppendListItem(xChanges->data->objList, entry))
			FreeEntryDataOwned(entry);
	}

	snapshot.entries.clear();
}

bool Cmd_ResurrectAll_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 count = 0;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell) return true;

	//resurrection mutates cell object lists, snapshot dead actor refids first,
	//then resurrect from the snapshot once iteration is done
	std::vector<UInt32> deadRefIDs;
	auto CollectCell = [&](TESObjectCELL* cell)
	{
		if (!cell) return;
		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* refr = iter.Get();
			if (!refr || refr == player) continue;

			UInt8 baseType = refr->baseForm ? refr->baseForm->typeID : 0;
			if (baseType != kFormType_Creature && baseType != kFormType_NPC) continue;

			if (((Actor*)refr)->lifeState != 2) continue;
			deadRefIDs.push_back(refr->refID);
		}
	};

	CollectCell(player->parentCell);

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
				//mask before shifting, negative cell coords make the raw shift formally UB
				UInt32 key = (((UInt32)(baseX + dx) & 0xFFFF) << 16) | ((UInt32)(baseY + dy) & 0xFFFF);
				CollectCell(world->cellMap->Lookup(key));
			}
		}
	}

	for (UInt32 refID : deadRefIDs)
	{
		auto* refr = (TESObjectREFR*)Engine::LookupFormByID(refID);
		if (!IsActorRef(refr)) continue;

		Actor* actor = (Actor*)refr;
		if (actor->lifeState != 2) continue;

		//clear 3D first so resurrection doesn't reuse dismembered model
		Engine::TESObjectREFR_Set3D(refr, nullptr, true);

		{
			BSSpinLockScope actorLock(GetProcessListsActorLock());
			ActorResurrect(actor, true, true, false);
		}

		//queue model reload
		Engine::ModelLoaderQueueReference(refr, 1, false);

		count++;
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
		Engine::TESObjectREFR_Set3D(thisObj, nullptr, true);
	}

	{
		BSSpinLockScope actorLock(GetProcessListsActorLock());
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

	Engine::ModelLoaderQueueReference(thisObj, 1, false);

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
