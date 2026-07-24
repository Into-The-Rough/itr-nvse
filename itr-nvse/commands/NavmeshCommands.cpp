//navmesh probes exposed as script commands
//GetPathLength reuses the standalone PathingRequest/Solution flow from PathingCommands
//IsPointOnNavmesh resolves a triangle via a stack PathingLocation
//GetCoverPointsInRadius walks the cell navmesh list and reads per-edge cover data

#include "NavmeshCommands.h"
#include "commands/PathingShared.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/RayCast.h"
#include "internal/GameGlobals.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <cmath>

extern const _ExtractArgs ExtractArgs;
extern NVSEArrayVarInterface* g_arrInterface;

namespace
{
	DWORD s_mainThreadId = 0;

	bool IsMainThread()
	{
		return !s_mainThreadId || GetCurrentThreadId() == s_mainThreadId;
	}

	bool IsActorRef(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm) return false;
		return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
	}

	float DistanceSq(const PathPoint3& a, const PathPoint3& b)
	{
		const float dx = a.x - b.x;
		const float dy = a.y - b.y;
		const float dz = a.z - b.z;
		return dx * dx + dy * dy + dz * dz;
	}

	float PathDistance(Actor* actor, TESObjectREFR* target)
	{
		if (!actor || !target || !target->parentCell)
			return -1.0f;

		ScopedPathingRequest request;
		ScopedPathingSolution solution;

		PathPoint3 destination = { target->posX, target->posY, target->posZ };
		CdeclCall<void>(0x9DBC90, actor, request.Get(), &destination, target->parentCell,
			target->parentCell->worldSpace, 0.0f, static_cast<void*>(nullptr)); //PathManager::BuildPath

		if (!CdeclCall<bool>(0x6D0900, request.Get(), solution.Get())) //PathManager::Solve
			return -1.0f;

		auto* solutionData = solution.Get();
		if (solutionData->incompletePath)
			return -1.0f;

		const UInt32 nodeCount = ThisCall<UInt32>(0x8B6800, solution.Get()); //PathingSolution::GetNodeCount

		float distance = 0.0f;
		PathPoint3 prev = {};
		bool havePrev = false;
		for (UInt32 i = 0; i < nodeCount; ++i)
		{
			auto* node = ThisCall<PathingNodeLayout*>(0x6E7970, solution.Get(), i); //PathingSolution::GetNode
			if (!node)
				break;
			const PathPoint3& pos = node->pathingLocation.location;
			if (havePrev)
			{
				const float dx = pos.x - prev.x;
				const float dy = pos.y - prev.y;
				const float dz = pos.z - prev.z;
				distance += sqrtf(dx * dx + dy * dy + dz * dz);
			}
			prev = pos;
			havePrev = true;
		}

		return havePrev ? distance : -1.0f;
	}

	bool PointOnNavmesh(const PathPoint3& point, TESObjectCELL* cell, TESWorldSpace* worldSpace)
	{
		if (!cell)
			return false;

		alignas(4) UInt8 loc[sizeof(PathingLocationLayout)] = {};
		ThisCall<void>(0x6DCEE0, loc, &point, cell, worldSpace); //PathingLocation::PathingLocation

		//stored navMeshInfo/navMeshes are borrowed engine pointers, no owned refs to release
		const bool resolved = ThisCall<bool>(0x6DD6F0, loc, 0); //PathingLocation::ResolveTriangle
		const UInt16 triangle = reinterpret_cast<PathingLocationLayout*>(loc)->triangle;
		return resolved && triangle != 0xFFFF;
	}

	struct EdgeEndpoints
	{
		const PathPoint3* p0;
		const PathPoint3* p1;
	};

	struct CoverPoint
	{
		PathPoint3 pos;
		UInt32 flags;
	};

	constexpr UInt32 kMaxCoverPoints = 128;

	void GatherCellCover(TESObjectCELL* cell, const PathPoint3& query, float radiusSq,
		CoverPoint* out, UInt32& count)
	{
		if (!cell || count >= kMaxCoverPoints)
			return;

		//cell navmesh list, resolves interior/exterior navmeshes for the cell
		void* container = CdeclCall<void*>(0x6D6F40, cell); //PathManager::GetCellNavMeshes
		if (!container)
			return;

		auto* navArr = reinterpret_cast<BSSimpleArrayLayout<void*>*>(container);
		for (UInt32 n = 0; n < navArr->size && count < kMaxCoverPoints; ++n)
		{
			void* navMesh = navArr->data ? navArr->data[n] : nullptr;
			if (!navMesh)
				continue;

			//triangle BSSimpleArray embedded at navMesh+0x38, data at +0x3C, size at +0x40, stride 0x10
			auto* triArr = reinterpret_cast<BSSimpleArrayLayout<UInt8>*>(reinterpret_cast<UInt8*>(navMesh) + 0x38);
			UInt8* triData = triArr->data;
			const UInt32 triCount = triArr->size;
			if (!triData)
				continue;

			for (UInt32 i = 0; i < triCount && i < 0x10000 && count < kMaxCoverPoints; ++i)
			{
				void* tri = triData + i * 0x10;
				if (!ThisCall<UInt32>(0x690770, tri)) //NavMeshTriangle::HasCover ((flags & 0x0FBE0000) != 0)
					continue;

				for (UInt32 slot = 0; slot < 2 && count < kMaxCoverPoints; ++slot)
				{
					UInt16 bucket = 0;
					bool leftOpen = false, rightOpen = false;
					ThisCall<void>(0x691040, tri, slot, &bucket, &leftOpen, &rightOpen); //NavMeshTriangle::GetEdgeCoverData

					if (!(bucket >= 2 || leftOpen || rightOpen))
						continue;

					EdgeEndpoints edge = {};
					ThisCall<void>(0x68F040, navMesh, &edge, i, slot); //NavMesh::GetEdgeEndpoints
					if (!edge.p0 || !edge.p1)
						continue;

					PathPoint3 mid = {
						(edge.p0->x + edge.p1->x) * 0.5f,
						(edge.p0->y + edge.p1->y) * 0.5f,
						(edge.p0->z + edge.p1->z) * 0.5f,
					};

					if (DistanceSq(mid, query) > radiusSq)
						continue;

					out[count].pos = mid;
					out[count].flags = (bucket & 0xF) | (leftOpen ? 0x10 : 0) | (rightOpen ? 0x20 : 0) | (slot << 6);
					++count;
				}
			}
		}
	}

	void GatherCoverAround(const PathPoint3& query, float radius, CoverPoint* out, UInt32& count)
	{
		PlayerCharacter* player = *g_thePlayerPtr;
		if (!player || !player->parentCell)
			return;

		const float radiusSq = radius * radius;
		TESObjectCELL* anchorCell = player->parentCell;
		if (anchorCell->IsInterior())
		{
			GatherCellCover(anchorCell, query, radiusSq, out, count);
		}
		else if (TESWorldSpace* world = anchorCell->worldSpace)
		{
			if (world->cellMap)
			{
				const SInt32 centreX = static_cast<SInt32>(floorf(query.x / 4096.0f));
				const SInt32 centreY = static_cast<SInt32>(floorf(query.y / 4096.0f));
				SInt32 cellRadius = static_cast<SInt32>(ceilf(radius / 4096.0f));
				if (cellRadius > 2) cellRadius = 2;

				for (SInt32 dx = -cellRadius; dx <= cellRadius && count < kMaxCoverPoints; ++dx)
				{
					for (SInt32 dy = -cellRadius; dy <= cellRadius && count < kMaxCoverPoints; ++dy)
					{
						//mask before shifting, negative cell coords make the raw shift formally UB
						const UInt32 key = (((UInt32)(centreX + dx) & 0xFFFF) << 16) | ((UInt32)(centreY + dy) & 0xFFFF);
						GatherCellCover(world->cellMap->Lookup(key), query, radiusSq, out, count);
					}
				}
			}
		}
	}

	constexpr UInt8 kLayerLineOfSight = 37; //LAYER_LINEOFSIGHT, engine vision filter
	constexpr float kThreatEyeHeight = 96.0f; //eye height approximation
	constexpr float kCoverTorsoHeight = 64.0f; //crouched torso height

	//true when the threat's view of the standing position is blocked, so the point is usable cover
	bool ThreatBlockedFromPoint(const PathPoint3& threat, const PathPoint3& cover)
	{
		RayCastData ray = {};
		ray.pos0[0] = threat.x * kHavokScale;
		ray.pos0[1] = threat.y * kHavokScale;
		ray.pos0[2] = (threat.z + kThreatEyeHeight) * kHavokScale;
		ray.pos1[0] = cover.x * kHavokScale;
		ray.pos1[1] = cover.y * kHavokScale;
		ray.pos1[2] = (cover.z + kCoverTorsoHeight) * kHavokScale;
		ray.hitFraction = 1.0f;
		ray.unk44[0] = 0xFFFFFFFF;
		ray.unk44[6] = 0xFFFFFFFF;
		ray.layerType = kLayerLineOfSight;

		//future: sample low/mid/high like the engine GetLineOfSight three-ray probe
		if (!Engine::TESPickObject(&ray, true))
			return false; //no cast performed, cannot confirm blockage, treat as exposed
		return ray.hitFraction < 1.0f;
	}

	constexpr UInt32 kMaxBestCover = 32;

	struct ScoredCover
	{
		PathPoint3 pos;
		UInt32 flags;
		float dist;
	};

	void InsertScored(ScoredCover* out, UInt32& outCount, UInt32 limit, const ScoredCover& item)
	{
		if (outCount >= limit && item.dist >= out[outCount - 1].dist)
			return;
		UInt32 pos = (outCount < limit) ? outCount : (outCount - 1);
		while (pos > 0 && out[pos - 1].dist > item.dist)
		{
			out[pos] = out[pos - 1];
			--pos;
		}
		out[pos] = item;
		if (outCount < limit)
			++outCount;
	}
}

static ParamInfo kParams_GetPathLength[2] = {
	{ "target", kParamType_ObjectRef, 0 },
	{ "source", kParamType_ObjectRef, 1 },
};

static ParamInfo kParams_IsPointOnNavmesh[4] = {
	{ "x",         kParamType_Float,     0 },
	{ "y",         kParamType_Float,     0 },
	{ "z",         kParamType_Float,     0 },
	{ "anchorRef", kParamType_ObjectRef, 1 },
};

static ParamInfo kParams_GetCoverPointsInRadius[4] = {
	{ "x",      kParamType_Float, 0 },
	{ "y",      kParamType_Float, 0 },
	{ "z",      kParamType_Float, 0 },
	{ "radius", kParamType_Float, 0 },
};

static ParamInfo kParams_GetBestCoverFromThreat[8] = {
	{ "threatX",    kParamType_Float,   0 },
	{ "threatY",    kParamType_Float,   0 },
	{ "threatZ",    kParamType_Float,   0 },
	{ "searchX",    kParamType_Float,   0 },
	{ "searchY",    kParamType_Float,   0 },
	{ "searchZ",    kParamType_Float,   0 },
	{ "radius",     kParamType_Float,   0 },
	{ "maxResults", kParamType_Integer, 1 },
};

DEFINE_COMMAND_PLUGIN(GetPathLength, "returns the complete path length from source (default calling ref) to target, or -1 on failure", 0, 2, kParams_GetPathLength);
DEFINE_COMMAND_PLUGIN(IsPointOnNavmesh, "returns 1 if the point resolves to a loaded navmesh triangle, else 0", 0, 4, kParams_IsPointOnNavmesh);
DEFINE_COMMAND_PLUGIN(GetCoverPointsInRadius, "returns array of [x,y,z,coverFlags] for cover edges near a point on loaded navmeshes (unsorted, max 128)", 0, 4, kParams_GetCoverPointsInRadius);
DEFINE_COMMAND_PLUGIN(GetBestCoverFromThreat, "returns array of [x,y,z,coverFlags,distToSearch] for cover near the search point that hides the standing position from the threat, sorted nearest-first (empty when none)", 0, 8, kParams_GetBestCoverFromThreat);

bool Cmd_GetPathLength_Execute(COMMAND_ARGS)
{
	*result = -1.0;

	TESObjectREFR* target = nullptr;
	TESObjectREFR* source = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target, &source))
		return true;

	if (!IsMainThread())
		return true;

	TESObjectREFR* actorRef = source ? source : thisObj;
	if (!target || !IsActorRef(actorRef))
		return true;

	*result = PathDistance(static_cast<Actor*>(actorRef), target);
	return true;
}

bool Cmd_IsPointOnNavmesh_Execute(COMMAND_ARGS)
{
	*result = 0;

	float x = 0, y = 0, z = 0;
	TESObjectREFR* anchorRef = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &x, &y, &z, &anchorRef))
		return true;

	if (!IsMainThread())
		return true;

	TESObjectCELL* cell = nullptr;
	if (anchorRef)
		cell = anchorRef->parentCell;
	else if (PlayerCharacter* player = *g_thePlayerPtr)
		cell = player->parentCell;

	if (!cell)
		return true;

	PathPoint3 point = { x, y, z };
	if (PointOnNavmesh(point, cell, cell->worldSpace))
		*result = 1;

	return true;
}

bool Cmd_GetCoverPointsInRadius_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!g_arrInterface)
		return true;

	float x = 0, y = 0, z = 0, radius = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &x, &y, &z, &radius))
		return true;

	if (!IsMainThread())
		return true;

	if (radius <= 0.0f || radius > 8192.0f)
		return true;

	const PathPoint3 query = { x, y, z };

	CoverPoint points[kMaxCoverPoints];
	UInt32 count = 0;
	GatherCoverAround(query, radius, points, count);

	NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	if (!arr)
		return true;
	for (UInt32 i = 0; i < count; ++i)
	{
		NVSEArrayVarInterface::Array* sub = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
		if (!sub)
			continue;
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(points[i].pos.x));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(points[i].pos.y));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(points[i].pos.z));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(static_cast<double>(points[i].flags)));
		g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(sub));
	}
	g_arrInterface->AssignCommandResult(arr, result);

	return true;
}

bool Cmd_GetBestCoverFromThreat_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!g_arrInterface)
		return true;

	float threatX = 0, threatY = 0, threatZ = 0;
	float searchX = 0, searchY = 0, searchZ = 0, radius = 0;
	SInt32 maxResults = 8;
	if (!ExtractArgs(EXTRACT_ARGS, &threatX, &threatY, &threatZ, &searchX, &searchY, &searchZ, &radius, &maxResults))
		return true;

	if (!IsMainThread())
		return true;

	if (radius <= 0.0f || radius > 8192.0f)
		return true;

	if (maxResults < 1) maxResults = 1;
	if (maxResults > static_cast<SInt32>(kMaxBestCover)) maxResults = kMaxBestCover;

	const PathPoint3 threat = { threatX, threatY, threatZ };
	const PathPoint3 search = { searchX, searchY, searchZ };

	CoverPoint points[kMaxCoverPoints];
	UInt32 count = 0;
	GatherCoverAround(search, radius, points, count);

	ScoredCover best[kMaxBestCover];
	UInt32 bestCount = 0;
	for (UInt32 i = 0; i < count; ++i)
	{
		if (!ThreatBlockedFromPoint(threat, points[i].pos))
			continue;
		ScoredCover item = { points[i].pos, points[i].flags, sqrtf(DistanceSq(points[i].pos, search)) };
		InsertScored(best, bestCount, static_cast<UInt32>(maxResults), item);
	}

	NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	if (!arr)
		return true;
	for (UInt32 i = 0; i < bestCount; ++i)
	{
		NVSEArrayVarInterface::Array* sub = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
		if (!sub)
			continue;
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(best[i].pos.x));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(best[i].pos.y));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(best[i].pos.z));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(static_cast<double>(best[i].flags)));
		g_arrInterface->AppendElement(sub, NVSEArrayVarInterface::Element(static_cast<double>(best[i].dist)));
		g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(sub));
	}
	g_arrInterface->AssignCommandResult(arr, result);

	return true;
}

namespace NavmeshCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		s_mainThreadId = GetCurrentThreadId();

		auto* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_GetPathLength);
		nvse->RegisterCommand(&kCommandInfo_IsPointOnNavmesh);
		nvse->RegisterTypedCommand(&kCommandInfo_GetCoverPointsInRadius, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetBestCoverFromThreat, kRetnType_Array);
	}
}
