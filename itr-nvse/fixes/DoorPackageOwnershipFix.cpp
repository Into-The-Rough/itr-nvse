//fixes NPCs with lock/unlock door packages locking doors they don't own
//vanilla bug: doors skip cell ownership inheritance in GetOwnerRawForm
//fix: hook IsAnOwner calls in lock/unlock functions to also check cell ownership for doors
//NOT hot-reloadable - requires game restart

#include "DoorPackageOwnershipFix.h"
#include "internal/EngineFunctions.h"
#include <Windows.h>
#include <cstdint>

namespace DoorPackageOwnershipFix
{
	typedef bool (__thiscall* _IsInFaction)(void* actor, void* faction);
	static _IsInFaction IsInFaction = (_IsInFaction)0x89A9A0;

	inline UInt8 GetFormTypeDirect(void* form) {
		return form ? *(UInt8*)((char*)form + 0x04) : 0;
	}

	//TESObjectREFR::parentCell is at offset 0x40
	inline void* GetParentCellDirect(void* refr)
	{
		return *(void**)((char*)refr + 0x40);
	}

	//TESObjectCELL flags2 at offset 0x26 - values 5 or 6 mean loaded
	inline bool IsCellLoaded(void* cell)
	{
		if (!cell) return false;
		UInt8 flags2 = *(UInt8*)((char*)cell + 0x26);
		return (flags2 >= 5);
	}

	//read cell owner directly from ExtraDataList without function calls
	//runs on AI thread - no lock on the ExtraData linked list. if main thread
	//modifies cell ownership mid-traversal (rare), a freed node could crash.
	//accepted: cell ownership changes mid-gameplay are extremely uncommon.
	void* GetCellOwnerDirect(void* cell)
	{
		if (!cell) return nullptr;

		void* extraHead = *(void**)((char*)cell + 0x28 + 0x04);

		//traverse linked list
		void* current = extraHead;
		while (current)
		{
			UInt8 type = *(UInt8*)((char*)current + 0x04);
			if (type == 0x21) //ExtraOwnership
			{
				return *(void**)((char*)current + 0x0C);
			}
			current = *(void**)((char*)current + 0x08);
		}

		return nullptr; //no owner
	}

	inline void* GetBaseForm(void* refr)
	{
		return refr ? *(void**)((char*)refr + 0x20) : nullptr;
	}

	//check if actor owns the cell (or is in the owning faction)
	bool ActorOwnsCell(void* actor, void* cellOwner)
	{
		if (!actor || !cellOwner) return true; //safe default

		//check if cell owner is a faction
		UInt8 ownerType = GetFormTypeDirect(cellOwner);
		if (ownerType == 0x06) //kFormType_Faction
		{
			return IsInFaction(actor, cellOwner);
		}

		//cell owner is an NPC - check if actor's base form matches
		//actor is a reference, owner is a base form
		void* actorBase = GetBaseForm(actor);
		if (!actorBase) return true;

		UInt32 actorBaseID = *(UInt32*)((char*)actorBase + 0x0C);
		UInt32 ownerID = *(UInt32*)((char*)cellOwner + 0x0C);
		return actorBaseID == ownerID;
	}

	//replacement IsAnOwner that also checks cell ownership for doors
	bool __fastcall IsAnOwner_Hook(void* refr, void* edx, void* actor, bool checkFaction)
	{
		bool originalResult = Engine::TESObjectREFR_IsAnOwner(refr, actor, checkFaction);

		if (!originalResult) return false;

		void* explicitOwner = Engine::TESObjectREFR_GetOwnerRawForm(refr);
		if (explicitOwner) return true;

		void* cell = GetParentCellDirect(refr);
		if (!cell) return true; //no cell = allow

		if (!IsCellLoaded(cell)) return true; //cell not loaded = skip check, allow

		void* cellOwner = GetCellOwnerDirect(cell);
		if (!cellOwner) return true; //no cell owner = anyone can use

		return ActorOwnsCell(actor, cellOwner);
	}

	void PatchMemory(UInt32 addr, const void* data, UInt32 size)
	{
		DWORD oldProtect;
		VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy((void*)addr, data, size);
		VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (void*)addr, size);
	}

	void ReplaceCall(UInt32 callAddr, UInt32 newFunc)
	{
		UInt32 relOffset = newFunc - callAddr - 5;
		UInt8 patch[5] = { 0xE8 }; //CALL opcode
		*(UInt32*)(patch + 1) = relOffset;
		PatchMemory(callAddr, patch, 5);
	}

	void Init()
	{
		ReplaceCall(0x90D528, (UInt32)IsAnOwner_Hook);  //LockDoorsAtLocation
		ReplaceCall(0x90D5DE, (UInt32)IsAnOwner_Hook);  //UnlockDoorsAtLocation
	}
}

