//Temporary path probes exposed as script commands.
//Builds a standalone PathingRequest/Solution instead of reading an actor's live mover path.

#include "PathingCommands.h"
#include "internal/CallTemplates.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <algorithm>
#include <cmath>
#include <vector>

extern const _ExtractArgs ExtractArgs;
extern NVSEArrayVarInterface* g_arrInterface;

namespace
{
	struct PathPoint3
	{
		float x;
		float y;
		float z;
	};
	static_assert(sizeof(PathPoint3) == 0x0C);

	struct PathingLocationLayout
	{
		void* vtbl;
		PathPoint3 location;
		void* navMeshInfo;
		void* navMeshes;
		TESObjectCELL* cell;
		TESWorldSpace* worldSpace;
		UInt32 cellCoords;
		UInt16 triangle;
		UInt8 flags;
		UInt8 clientData;
	};
	static_assert(sizeof(PathingLocationLayout) == 0x28);

	struct PathingNodeLayout
	{
		UInt32 flags;
		PathingLocationLayout pathingLocation;
		PathPoint3 tangent;
		TESObjectREFR* actionRef;
	};
	static_assert(sizeof(PathingNodeLayout) == 0x3C);

	template <typename T>
	struct BSSimpleArrayLayout
	{
		void* vtbl;
		T* data;
		UInt32 size;
		UInt32 allocSize;
	};
	static_assert(sizeof(BSSimpleArrayLayout<void>) == 0x10);

	struct PathingSolutionLayout
	{
		void* vtbl;
		UInt32 refCount;
		BSSimpleArrayLayout<void> virtualNodes;
		SInt32 firstLoadedVirtualNodeIndex;
		SInt32 lastLoadedVirtualNodeIndex;
		BSSimpleArrayLayout<PathingNodeLayout> currentNodes;
		BSSimpleArrayLayout<UInt32> previousNodes;
		UInt8 incompletePath;
	};
	static_assert(sizeof(PathingSolutionLayout) == 0x44);

	struct ScopedPathingRequest
	{
		alignas(4) UInt8 data[0xB0] = {};

		ScopedPathingRequest()
		{
			ThisCall<void>(0x6E2420, data);
		}

		~ScopedPathingRequest()
		{
			ThisCall<void>(0x6E2620, data);
		}

		void* Get()
		{
			return data;
		}
	};

	struct ScopedPathingSolution
	{
		alignas(4) UInt8 data[sizeof(PathingSolutionLayout)] = {};

		ScopedPathingSolution()
		{
			ThisCall<void>(0x6E7650, data);
		}

		~ScopedPathingSolution()
		{
			ThisCall<void>(0x6E7720, data);
		}

		PathingSolutionLayout* Get()
		{
			return reinterpret_cast<PathingSolutionLayout*>(data);
		}
	};

	struct PathResult
	{
		bool complete = false;
		float distance = -1.0f;
		std::vector<PathPoint3> nodes;
	};

	struct CachedPathResult
	{
		bool hasValue = false;
		UInt32 actorRefID = 0;
		UInt32 targetRefID = 0;
		DWORD tick = 0;
		PathResult path;
	};

	constexpr UInt32 kMaxPathNodes = 1024;
	constexpr DWORD kCacheTtlMs = 100;

	DWORD s_mainThreadId = 0;
	CachedPathResult s_cachedPath;

	bool IsActorRef(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm) return false;
		return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
	}

	bool IsMainThread()
	{
		return !s_mainThreadId || GetCurrentThreadId() == s_mainThreadId;
	}

	UInt32 GetRefKey(TESObjectREFR* ref)
	{
		return ref->refID ? ref->refID : reinterpret_cast<UInt32>(ref);
	}

	float Distance(const PathPoint3& a, const PathPoint3& b)
	{
		const float dx = a.x - b.x;
		const float dy = a.y - b.y;
		const float dz = a.z - b.z;
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

	float ComputeDistance(const std::vector<PathPoint3>& nodes)
	{
		float distance = 0.0f;
		for (size_t i = 1; i < nodes.size(); ++i)
			distance += Distance(nodes[i - 1], nodes[i]);
		return distance;
	}

	bool BuildPath(Actor* actor, TESObjectREFR* target, PathResult& out)
	{
		out = PathResult();

		if (!actor || !target || !target->parentCell)
			return false;

		ScopedPathingRequest request;
		ScopedPathingSolution solution;

		PathPoint3 destination = { target->posX, target->posY, target->posZ };
		CdeclCall<void>(
			0x9DBC90,
			actor,
			request.Get(),
			&destination,
			target->parentCell,
			target->parentCell->worldSpace,
			0.0f,
			static_cast<void*>(nullptr));

		if (!CdeclCall<bool>(0x6D0900, request.Get(), solution.Get()))
			return false;

		auto* solutionData = solution.Get();
		if (solutionData->incompletePath)
			return false;

		const UInt32 nodeCount = ThisCall<UInt32>(0x8B6800, solution.Get());
		const UInt32 safeCount = (std::min)(nodeCount, kMaxPathNodes);
		out.nodes.reserve(safeCount);

		for (UInt32 i = 0; i < safeCount; ++i)
		{
			auto* node = ThisCall<PathingNodeLayout*>(0x6E7970, solution.Get(), i);
			if (!node)
				break;
			out.nodes.push_back(node->pathingLocation.location);
		}

		if (out.nodes.empty())
			return false;

		out.distance = ComputeDistance(out.nodes);
		out.complete = true;
		return true;
	}

	bool GetPath(TESObjectREFR* actorRef, TESObjectREFR* target, PathResult& out)
	{
		out = PathResult();

		if (!IsMainThread() || !actorRef || !target || !IsActorRef(actorRef))
			return false;

		const DWORD now = GetTickCount();
		const UInt32 actorKey = GetRefKey(actorRef);
		const UInt32 targetKey = GetRefKey(target);

		if (s_cachedPath.hasValue &&
			s_cachedPath.actorRefID == actorKey &&
			s_cachedPath.targetRefID == targetKey &&
			now - s_cachedPath.tick <= kCacheTtlMs)
		{
			out = s_cachedPath.path;
			return out.complete;
		}

		s_cachedPath.hasValue = true;
		s_cachedPath.actorRefID = actorKey;
		s_cachedPath.targetRefID = targetKey;
		s_cachedPath.tick = now;
		s_cachedPath.path = PathResult();

		BuildPath(static_cast<Actor*>(actorRef), target, s_cachedPath.path);
		out = s_cachedPath.path;
		return out.complete;
	}

	NVSEArrayVarInterface::Array* CreateArray(Script* scriptObj)
	{
		if (!g_arrInterface)
			return nullptr;
		return g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	}

	void AssignArray(NVSEArrayVarInterface::Array* arr, double* result)
	{
		if (g_arrInterface && arr)
			g_arrInterface->AssignCommandResult(arr, result);
	}

	void AppendPointComponents(NVSEArrayVarInterface::Array* arr, const PathPoint3& point)
	{
		g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(point.x));
		g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(point.y));
		g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(point.z));
	}

	void AppendPointArray(NVSEArrayVarInterface::Array* outer, const PathPoint3& point, Script* scriptObj)
	{
		auto* pointArr = CreateArray(scriptObj);
		if (!pointArr)
			return;

		AppendPointComponents(pointArr, point);
		g_arrInterface->AppendElement(outer, NVSEArrayVarInterface::Element(pointArr));
	}
}

bool Cmd_CanPathToRef_Execute(COMMAND_ARGS)
{
	*result = 0;

	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target))
		return true;

	PathResult path;
	if (GetPath(thisObj, target, path))
		*result = 1;

	return true;
}

bool Cmd_GetPathDistanceToRef_Execute(COMMAND_ARGS)
{
	*result = -1.0;

	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target))
		return true;

	PathResult path;
	if (GetPath(thisObj, target, path))
		*result = path.distance;

	return true;
}

bool Cmd_GetPathNodeCount_Execute(COMMAND_ARGS)
{
	*result = 0;

	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target))
		return true;

	PathResult path;
	if (GetPath(thisObj, target, path))
		*result = static_cast<double>(path.nodes.size());

	return true;
}

bool Cmd_GetNthPathNode_Execute(COMMAND_ARGS)
{
	*result = 0;

	Actor* target = nullptr;
	UInt32 index = 0;
	auto* arr = CreateArray(scriptObj);

	if (ExtractArgs(EXTRACT_ARGS, &target, &index))
	{
		PathResult path;
		if (GetPath(thisObj, target, path) && index < path.nodes.size() && arr)
			AppendPointComponents(arr, path.nodes[index]);
	}

	AssignArray(arr, result);
	return true;
}

bool Cmd_GetPathToRef_Execute(COMMAND_ARGS)
{
	*result = 0;

	Actor* target = nullptr;
	auto* arr = CreateArray(scriptObj);

	if (ExtractArgs(EXTRACT_ARGS, &target))
	{
		PathResult path;
		if (GetPath(thisObj, target, path) && arr)
		{
			for (const auto& node : path.nodes)
				AppendPointArray(arr, node, scriptObj);
		}
	}

	AssignArray(arr, result);
	return true;
}

static ParamInfo kParams_OneTargetRef[1] = {
	{ "target", kParamType_Actor, 0 },
};

static ParamInfo kParams_TargetRef_OneIndex[2] = {
	{ "target", kParamType_Actor, 0 },
	{ "index", kParamType_Integer, 0 },
};

DEFINE_COMMAND_PLUGIN(CanPathToRef, "returns 1 if the calling actor can build a complete path to target", true, 1, kParams_OneTargetRef);
DEFINE_COMMAND_PLUGIN(GetPathDistanceToRef, "returns complete path distance to target, or -1 on failure", true, 1, kParams_OneTargetRef);
DEFINE_COMMAND_PLUGIN(GetPathNodeCount, "returns waypoint count for a complete path to target", true, 1, kParams_OneTargetRef);
DEFINE_COMMAND_PLUGIN(GetNthPathNode, "returns [x,y,z] for waypoint index in the complete path to target", true, 2, kParams_TargetRef_OneIndex);
DEFINE_COMMAND_PLUGIN(GetPathToRef, "returns [[x,y,z], ...] for the complete path to target", true, 1, kParams_OneTargetRef);

namespace PathingCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		s_mainThreadId = GetCurrentThreadId();

		auto* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_CanPathToRef);
		nvse->RegisterCommand(&kCommandInfo_GetPathDistanceToRef);
		nvse->RegisterCommand(&kCommandInfo_GetPathNodeCount);
		nvse->RegisterTypedCommand(&kCommandInfo_GetNthPathNode, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPathToRef, kRetnType_Array);
	}
}
