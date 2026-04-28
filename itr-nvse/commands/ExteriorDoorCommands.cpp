#include "ExteriorDoorCommands.h"
#include "internal/CallTemplates.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"

#include <algorithm>
#include <vector>

extern const _ExtractArgs ExtractArgs;

namespace
{
	using DoorTeleportData = ExtraTeleport::Data;

	static TESObjectCELL* GetParentCell(TESObjectREFR* ref)
	{
		if (!ref)
			return nullptr;

		if (ref->parentCell)
			return ref->parentCell;

		return ThisCall<TESObjectCELL*>(0x41D460, &ref->extraDataList);
	}

	static bool GetDisabled(TESForm* form)
	{
		return form && ThisCall<bool>(0x440DA0, form);
	}

	static bool GetDeleted(TESForm* form)
	{
		return form && ThisCall<bool>(0x440D80, form);
	}

	static DoorTeleportData* GetTeleport(TESObjectREFR* ref)
	{
		return ref ? ThisCall<DoorTeleportData*>(0x568E50, ref) : nullptr;
	}

	static TESObjectCELL* GetTeleportCell(DoorTeleportData* teleport)
	{
		return teleport ? ThisCall<TESObjectCELL*>(0x43A2B0, teleport) : nullptr;
	}

	static TESWorldSpace* GetTeleportWorldSpace(DoorTeleportData* teleport)
	{
		return teleport ? ThisCall<TESWorldSpace*>(0x43A320, teleport) : nullptr;
	}

	class ScopedCellRefLock
	{
	public:
		explicit ScopedCellRefLock(TESObjectCELL* cell) : cell_(cell)
		{
			if (cell_)
				ThisCall<void>(0x541AC0, cell_);
		}

		~ScopedCellRefLock()
		{
			if (cell_)
				ThisCall<void>(0x541AE0, cell_);
		}

		ScopedCellRefLock(const ScopedCellRefLock&) = delete;
		ScopedCellRefLock& operator=(const ScopedCellRefLock&) = delete;

	private:
		TESObjectCELL* cell_;
	};

	static bool ContainsCell(const std::vector<TESObjectCELL*>& cells, TESObjectCELL* cell)
	{
		return std::find(cells.begin(), cells.end(), cell) != cells.end();
	}

	static bool IsUsableLoadDoor(TESObjectREFR* ref)
	{
		if (!ref || GetDisabled(ref) || GetDeleted(ref))
			return false;

		TESForm* baseForm = ref->baseForm;
		if (!baseForm || baseForm->typeID != kFormType_Door)
			return false;

		return GetTeleport(ref) != nullptr;
	}

	static void CollectLoadDoors(TESObjectCELL* cell, std::vector<TESObjectREFR*>& doors)
	{
		if (!cell)
			return;

		ScopedCellRefLock lock(cell);

		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* ref = iter.Get();
			if (IsUsableLoadDoor(ref))
				doors.push_back(ref);
		}
	}

	static TESObjectREFR* FindExteriorDoor(TESObjectCELL* startCell)
	{
		std::vector<TESObjectCELL*> queue;
		std::vector<TESObjectCELL*> visited;
		queue.push_back(startCell);

		for (std::size_t index = 0; index < queue.size(); ++index)
		{
			TESObjectCELL* cell = queue[index];
			if (!cell || ContainsCell(visited, cell))
				continue;

			visited.push_back(cell);

			std::vector<TESObjectREFR*> doors;
			CollectLoadDoors(cell, doors);

			for (TESObjectREFR* door : doors)
			{
				DoorTeleportData* teleport = GetTeleport(door);
				if (!teleport || !teleport->linkedDoor)
					continue;

				TESObjectCELL* linkedCell = GetParentCell(teleport->linkedDoor);
				if (!linkedCell)
					linkedCell = GetTeleportCell(teleport);

				TESWorldSpace* linkedWorld = GetTeleportWorldSpace(teleport);
				if (!linkedCell || !linkedCell->IsInterior() || linkedWorld)
					return teleport->linkedDoor;

				if (linkedCell && linkedCell->IsInterior() &&
					!ContainsCell(visited, linkedCell) && !ContainsCell(queue, linkedCell))
				{
					queue.push_back(linkedCell);
				}
			}
		}

		return nullptr;
	}

	static ParamInfo kParams_GetRefExteriorDoor[1] =
	{
		{ "refr", kParamType_AnyForm, 1 },
	};

	DEFINE_COMMAND_PLUGIN(GetRefExteriorDoor, "returns the exterior-side load door reachable from a reference", 0, 1, kParams_GetRefExteriorDoor);

	bool Cmd_GetRefExteriorDoor_Execute(COMMAND_ARGS)
	{
		UInt32* refResult = reinterpret_cast<UInt32*>(result);
		*refResult = 0;

		TESForm* explicitForm = nullptr;
		ExtractArgs(EXTRACT_ARGS, &explicitForm);

		TESObjectREFR* targetRef = thisObj;
		if (explicitForm)
		{
			if (!explicitForm->IsReference())
				return true;

			targetRef = static_cast<TESObjectREFR*>(explicitForm);
		}

		if (!targetRef)
			return true;

		TESObjectCELL* parentCell = GetParentCell(targetRef);
		if (!parentCell)
			return true;

		if (!parentCell->IsInterior())
		{
			*refResult = targetRef->refID;
			return true;
		}

		if (TESObjectREFR* exteriorDoor = FindExteriorDoor(parentCell))
			*refResult = exteriorDoor->refID;

		return true;
	}
}

namespace ExteriorDoorCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterTypedCommand(&kCommandInfo_GetRefExteriorDoor, kRetnType_Form);
	}
}
