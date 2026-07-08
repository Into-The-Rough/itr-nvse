//fixes NPCs with lock/unlock door packages locking doors they don't own
//vanilla bug: doors skip cell ownership inheritance in GetOwnerRawForm
//fix: hook IsAnOwner calls in lock/unlock functions to also check cell ownership for doors
//NOT hot-reloadable - requires game restart

#include "DoorPackageOwnershipFix.h"
#include "internal/EngineFunctions.h"
#include "internal/Detours.h"
#include "internal/GameSDK.h"

#include <cstdint>

namespace DoorPackageOwnershipFix
{
	static Detours::CallDetour s_lockDoorsIsOwnerCall;
	static Detours::CallDetour s_unlockDoorsIsOwnerCall;

	inline bool IsCellLoaded(TESObjectCELL* cell)
	{
		if (!cell) return false;
		return cell->flags2 >= 5;
	}

	TESForm* GetCellOwner(TESObjectCELL* cell)
	{
		if (!cell) return nullptr;
		auto* ownership = static_cast<ExtraOwnership*>(
			Engine::BaseExtraList_GetByType(&cell->extraDataList, kExtraData_Ownership));
		return ownership ? ownership->owner : nullptr;
	}

	//actor's base is in the faction if its faction list holds a matching entry
	bool ActorInFaction(TESActorBase* actorBase, TESForm* faction)
	{
		auto* node = actorBase->baseData.factionList.Head();
		while (node && node->item)
		{
			if (node->item->faction == faction)
				return true;
			node = node->next;
		}
		return false;
	}

	//check if actor owns the cell (or is in the owning faction)
	bool ActorOwnsCell(Actor* actor, TESForm* cellOwner)
	{
		if (!actor || !cellOwner) return true; //safe default

		auto* actorBase = static_cast<TESActorBase*>(actor->baseForm);
		if (!actorBase) return true;

		if (cellOwner->typeID == kFormType_Faction)
			return ActorInFaction(actorBase, cellOwner);

		//cell owner is an NPC, compare against the actor's base form
		return actorBase->refID == cellOwner->refID;
	}

	//replacement IsAnOwner that also checks cell ownership for doors
	bool __fastcall IsAnOwner_Hook(TESObjectREFR* refr, void* edx, Actor* actor, bool checkFaction)
	{
		bool originalResult = Engine::TESObjectREFR_IsAnOwner(refr, actor, checkFaction);

		if (!originalResult) return false;

		TESForm* explicitOwner = static_cast<TESForm*>(Engine::TESObjectREFR_GetOwnerRawForm(refr));
		if (explicitOwner) return true;

		TESObjectCELL* cell = refr ? refr->parentCell : nullptr;
		if (!cell) return true; //no cell = allow

		if (!IsCellLoaded(cell)) return true; //cell not loaded = skip check, allow

		TESForm* cellOwner = GetCellOwner(cell);
		if (!cellOwner) return true; //no cell owner = anyone can use

		return ActorOwnsCell(actor, cellOwner);
	}

	void Init()
	{
		s_lockDoorsIsOwnerCall.WriteRelCall(0x90D528, IsAnOwner_Hook);  //LockDoorsAtLocation
		s_unlockDoorsIsOwnerCall.WriteRelCall(0x90D5DE, IsAnOwner_Hook);  //UnlockDoorsAtLocation
	}
}
