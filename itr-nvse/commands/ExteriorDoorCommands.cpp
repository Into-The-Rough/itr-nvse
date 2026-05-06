#include "ExteriorDoorCommands.h"
#include "internal/CallTemplates.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

extern const _ExtractArgs ExtractArgs;

namespace
{
	using DoorTeleportData = ExtraTeleport::Data;

	struct TeleportPathData
	{
		BSSimpleArray<BGSQuestObjective::ParentSpaceNode> parentSpaceNodes;
		BSSimpleArray<BGSQuestObjective::TeleportLink> teleportLinks;
		float startPos[3];
		float endPos[3];
	};

	static_assert(sizeof(TeleportPathData) == 0x38, "TeleportPathData must match engine TeleportPath");

	class ScopedTeleportPathData
	{
	public:
		ScopedTeleportPathData()
		{
			ThisCall<void>(0x6F48B0, &data_);
		}

		~ScopedTeleportPathData()
		{
			ThisCall<void>(0x6F4930, &data_);
		}

		ScopedTeleportPathData(const ScopedTeleportPathData&) = delete;
		ScopedTeleportPathData& operator=(const ScopedTeleportPathData&) = delete;

		TeleportPathData* Get() { return &data_; }

	private:
		TeleportPathData data_;
	};

	struct TravelSpace
	{
		bool isWorldspace = false;
		TESWorldSpace* worldspace = nullptr;
		TESObjectCELL* cell = nullptr;
	};

	struct RouteNode
	{
		TravelSpace space;
		float cost = 0.0f;
		float arrival[3] = {};
		TESObjectREFR* firstDoor = nullptr;
		bool visited = false;
	};

	struct FallbackRoute
	{
		TravelSpace targetSpace;
		TESObjectREFR* door = nullptr;
	};

	struct RouteCache
	{
		bool valid = false;
		TravelSpace startSpace;
		std::vector<RouteNode> nodes;
		std::vector<TESObjectREFR*> doors;
		std::vector<FallbackRoute> fallbacks;
	};

	static RouteCache s_routeCache;

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

	static bool IsValidTravelSpace(const TravelSpace& space)
	{
		return space.isWorldspace ? space.worldspace != nullptr : space.cell != nullptr;
	}

	static bool IsSameTravelSpace(const TravelSpace& lhs, const TravelSpace& rhs)
	{
		return lhs.isWorldspace == rhs.isWorldspace &&
		       lhs.worldspace == rhs.worldspace &&
		       lhs.cell == rhs.cell;
	}

	static TravelSpace GetTravelSpace(TESObjectCELL* cell)
	{
		if (!cell)
			return {};

		if (cell->IsInterior())
			return { false, nullptr, cell };

		return { true, cell->worldSpace, nullptr };
	}

	static TravelSpace GetTeleportDestinationSpace(DoorTeleportData* teleport)
	{
		if (TESWorldSpace* worldspace = GetTeleportWorldSpace(teleport))
			return { true, worldspace, nullptr };

		return GetTravelSpace(GetTeleportCell(teleport));
	}

	static float GetDistance(float x1, float y1, float z1, float x2, float y2, float z2)
	{
		const float x = x1 - x2;
		const float y = y1 - y2;
		const float z = z1 - z2;
		return std::sqrt((x * x) + (y * y) + (z * z));
	}

	static float GetDoorPenalty(TESObjectREFR* door)
	{
		if (!door || !door->baseForm || door->baseForm->typeID != kFormType_Door)
			return 0.0f;

		return ThisCall<bool>(0x518000, door->baseForm) ? 409600.0f : 0.0f;
	}

	static void ClearRouteCache()
	{
		s_routeCache.valid = false;
		s_routeCache.startSpace = {};
		s_routeCache.nodes.clear();
		s_routeCache.doors.clear();
		s_routeCache.fallbacks.clear();
	}

	static SInt32 FindRouteNode(const TravelSpace& space)
	{
		for (std::size_t i = 0; i < s_routeCache.nodes.size(); ++i)
		{
			if (IsSameTravelSpace(s_routeCache.nodes[i].space, space))
				return static_cast<SInt32>(i);
		}

		return -1;
	}

	static SInt32 FindBestOpenRouteNode()
	{
		SInt32 bestIndex = -1;
		float bestCost = (std::numeric_limits<float>::max)();

		for (std::size_t i = 0; i < s_routeCache.nodes.size(); ++i)
		{
			const RouteNode& node = s_routeCache.nodes[i];
			if (!node.visited && node.cost < bestCost)
			{
				bestIndex = static_cast<SInt32>(i);
				bestCost = node.cost;
			}
		}

		return bestIndex;
	}

	static void AddOrUpdateRouteNode(const TravelSpace& space, float cost, const float arrival[3], TESObjectREFR* firstDoor)
	{
		if (!IsValidTravelSpace(space) || !firstDoor)
			return;

		const SInt32 existingIndex = FindRouteNode(space);
		if (existingIndex >= 0)
		{
			RouteNode& existing = s_routeCache.nodes[existingIndex];
			if (!existing.visited && cost < existing.cost)
			{
				existing.cost = cost;
				existing.arrival[0] = arrival[0];
				existing.arrival[1] = arrival[1];
				existing.arrival[2] = arrival[2];
				existing.firstDoor = firstDoor;
			}
			return;
		}

		RouteNode node;
		node.space = space;
		node.cost = cost;
		node.arrival[0] = arrival[0];
		node.arrival[1] = arrival[1];
		node.arrival[2] = arrival[2];
		node.firstDoor = firstDoor;
		s_routeCache.nodes.push_back(node);
	}

	static bool GetFallbackRoute(const TravelSpace& targetSpace, TESObjectREFR*& door)
	{
		if (!IsValidTravelSpace(targetSpace))
			return false;

		for (const FallbackRoute& route : s_routeCache.fallbacks)
		{
			if (IsSameTravelSpace(route.targetSpace, targetSpace))
			{
				door = route.door;
				return true;
			}
		}

		return false;
	}

	static void CacheFallbackRoute(const TravelSpace& targetSpace, TESObjectREFR* door)
	{
		if (!IsValidTravelSpace(targetSpace))
			return;

		s_routeCache.fallbacks.push_back({ targetSpace, door });
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

	static void CollectLoadDoors(const TravelSpace& space, std::vector<TESObjectREFR*>& doors)
	{
		if (space.isWorldspace)
		{
			if (space.worldspace)
				CollectLoadDoors(space.worldspace->cell, doors);
			return;
		}

		CollectLoadDoors(space.cell, doors);
	}

	static bool EnsureRouteCache(PlayerCharacter* player, TESObjectCELL* playerCell)
	{
		TravelSpace startSpace = GetTravelSpace(playerCell);
		if (!player || !IsValidTravelSpace(startSpace))
			return false;

		if (s_routeCache.valid && IsSameTravelSpace(s_routeCache.startSpace, startSpace))
			return true;

		ClearRouteCache();
		s_routeCache.valid = true;
		s_routeCache.startSpace = startSpace;

		RouteNode startNode;
		startNode.space = startSpace;
		startNode.arrival[0] = player->posX;
		startNode.arrival[1] = player->posY;
		startNode.arrival[2] = player->posZ;
		s_routeCache.nodes.push_back(startNode);
		return true;
	}

	static void ExpandRouteNode(SInt32 nodeIndex)
	{
		if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= s_routeCache.nodes.size())
			return;

		RouteNode node = s_routeCache.nodes[nodeIndex];
		s_routeCache.doors.clear();
		CollectLoadDoors(node.space, s_routeCache.doors);

		for (TESObjectREFR* door : s_routeCache.doors)
		{
			DoorTeleportData* teleport = GetTeleport(door);
			TravelSpace destination = GetTeleportDestinationSpace(teleport);
			if (!IsValidTravelSpace(destination) || IsSameTravelSpace(destination, node.space))
				continue;

			float arrival[3] = { teleport->x, teleport->y, teleport->z };
			float cost = node.cost +
			             GetDistance(node.arrival[0], node.arrival[1], node.arrival[2], door->posX, door->posY, door->posZ) +
			             GetDoorPenalty(door);
			TESObjectREFR* firstDoor = node.firstDoor ? node.firstDoor : door;
			AddOrUpdateRouteNode(destination, cost, arrival, firstDoor);
		}
	}

	static TESObjectREFR* FindCachedRouteDoor(PlayerCharacter* player, TESObjectCELL* playerCell, const TravelSpace& targetSpace)
	{
		if (!EnsureRouteCache(player, playerCell) || !IsValidTravelSpace(targetSpace))
			return nullptr;

		while (true)
		{
			SInt32 targetIndex = FindRouteNode(targetSpace);
			if (targetIndex >= 0 && s_routeCache.nodes[targetIndex].visited)
				return s_routeCache.nodes[targetIndex].firstDoor;

			SInt32 nextIndex = FindBestOpenRouteNode();
			if (nextIndex < 0)
				return nullptr;

			s_routeCache.nodes[nextIndex].visited = true;
			if (IsSameTravelSpace(s_routeCache.nodes[nextIndex].space, targetSpace))
				return s_routeCache.nodes[nextIndex].firstDoor;

			ExpandRouteNode(nextIndex);
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
	DEFINE_COMMAND_PLUGIN(GetRefNextTeleportDoor, "returns the next load door on the current shortest path to a reference", 0, 1, kParams_GetRefExteriorDoor);

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

	bool Cmd_GetRefNextTeleportDoor_Execute(COMMAND_ARGS)
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

		PlayerCharacter* player = PlayerCharacter::GetSingleton();
		if (!player || !targetRef)
			return true;

		TESObjectCELL* playerCell = GetParentCell(player);
		TESObjectCELL* targetCell = GetParentCell(targetRef);
		if (!playerCell || !targetCell)
			return true;

		TravelSpace targetSpace = GetTravelSpace(targetCell);

		//same-space paths contain no teleport links, so the next thing to point at is the target
		if (IsSameTravelSpace(GetTravelSpace(playerCell), targetSpace))
		{
			*refResult = targetRef->refID;
			return true;
		}

		if (TESObjectREFR* cachedDoor = FindCachedRouteDoor(player, playerCell, targetSpace))
		{
			*refResult = cachedDoor->refID;
			return true;
		}

		TESObjectREFR* fallbackDoor = nullptr;
		if (GetFallbackRoute(targetSpace, fallbackDoor))
		{
			if (fallbackDoor)
				*refResult = fallbackDoor->refID;
			return true;
		}

		ScopedTeleportPathData path;
		ThisCall<void>(0x952D60, player, targetRef, path.Get(), 1);

		TeleportPathData* pathData = path.Get();
		if (pathData->teleportLinks.size == 0 || !pathData->teleportLinks.data)
		{
			CacheFallbackRoute(targetSpace, nullptr);
			return true;
		}

		TESObjectREFR* nextDoor = pathData->teleportLinks.data[0].door;
		if (nextDoor)
			*refResult = nextDoor->refID;
		CacheFallbackRoute(targetSpace, nextDoor);

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

	void RegisterCommands2(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterTypedCommand(&kCommandInfo_GetRefNextTeleportDoor, kRetnType_Form);
	}

	void AdvanceFrameCache()
	{
		ClearRouteCache();
	}

	void ClearCache()
	{
		ClearRouteCache();
	}
}
