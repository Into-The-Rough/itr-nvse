//reads the navmesh triangle an actor is standing on plus its cover data, and enriches that with
//the live CombatProcedureBeInCover when the AI happens to be running a from-cover action
//intended for animation mods that need to know when an actor enters, peeks from or leaves cover

#include "CoverCommands.h"
#include "commands/PathingShared.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/layout/Combat.h"

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <vector>

extern const _ExtractArgs ExtractArgs;
extern NVSEArrayVarInterface* g_arrInterface;

namespace
{
	using Element = NVSEArrayVarInterface::Element;
	using Array = NVSEArrayVarInterface::Array;

	constexpr UInt32 kProcedureType_BeInCover = 7; //0x9D2510, the type getter in vtbl slot 13
	constexpr UInt32 kVtbl_CombatProcedureBeInCover = 0x10910F4;
	constexpr UInt32 kTriangleFlag_Disabled = 0x20;

	using GetProcedureByType_t = void* (__thiscall*)(void*, UInt32);
	const auto CombatControllerGetProcedureByType = reinterpret_cast<GetProcedureByType_t>(0x980400);

	//0x68F040 hands back pointers into the navmesh vertex array, not copies
	struct EdgeEndpoints
	{
		const PathPoint3* p0;
		const PathPoint3* p1;
	};

	const char* kCoverStateNames[kCoverState_Count] = {
		"INITIALIZING",
		"WAITING_BEHIND_COVER",
		"MOVING_OUT",
		"WAITING_OUT_OF_COVER",
		"FIRING_OUT_OF_COVER",
		"MOVING_IN",
		"MOVING_IN_AND_ROTATE",
		"HOLDING_GROUND",
	};

	bool IsActorRef(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm) return false;
		return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
	}

	bool IsBeInCover(void* proc)
	{
		return proc && *reinterpret_cast<UInt32*>(proc) == kVtbl_CombatProcedureBeInCover;
	}

	//the cover action parks the procedure in a controller slot, not in the array 0x980400 walks,
	//so the slots have to be checked first
	void* FindCoverProcedure(void* controller)
	{
		if (!controller) return nullptr;
		auto* view = reinterpret_cast<CombatControllerView*>(controller);
		if (IsBeInCover(view->movementProcedure)) return view->movementProcedure;
		if (IsBeInCover(view->attackProcedure)) return view->attackProcedure;
		return CombatControllerGetProcedureByType(controller, kProcedureType_BeInCover);
	}

	//the cover the combat state has reserved, which outlives any single from-cover action
	void* GetReservedCoverLocation(void* controller)
	{
		if (!controller) return nullptr;
		void* state = reinterpret_cast<CombatControllerView*>(controller)->combatState;
		if (!state) return nullptr;
		auto* view = reinterpret_cast<CombatStateView*>(state);
		return view->coverLocationA ? view->coverLocationA : view->coverLocationB;
	}

	//the triangle the actor is standing on, resolved from its position with no combat state involved
	bool ResolveActorTriangle(TESObjectREFR* ref, ScopedNavMeshPtr& navMesh, UInt16& triangle)
	{
		if (!ref || !ref->parentCell) return false;

		const PathPoint3 point = { ref->posX, ref->posY, ref->posZ };
		alignas(4) UInt8 loc[sizeof(PathingLocationLayout)] = {};
		ThisCall<void>(0x6DCEE0, loc, &point, ref->parentCell, ref->parentCell->worldSpace); //PathingLocation ctor

		//stored navMeshInfo/navMeshes are borrowed engine pointers, the NavMeshPtr below is the owned one
		if (!ThisCall<bool>(0x6DD6F0, loc, 0)) //PathingLocation::ResolveTriangle
			return false;
		return ThisCall<bool>(0x6DD640, loc, navMesh.Slot(), &triangle);
	}

	void* GetTriangle(void* navMesh, UInt16 index)
	{
		if (!navMesh) return nullptr;
		auto* triArr = reinterpret_cast<BSSimpleArrayLayout<UInt8>*>(static_cast<UInt8*>(navMesh) + 0x38);
		if (!triArr->data || index >= triArr->size) return nullptr;
		return triArr->data + index * 0x10;
	}

	void AddKey(std::vector<const char*>& keys, std::vector<Element>& values, const char* key, const Element& value)
	{
		keys.push_back(key);
		values.push_back(value);
	}

	Array* MakeVector(const float* v, Script* scriptObj)
	{
		const Element data[3] = { Element((double)v[0]), Element((double)v[1]), Element((double)v[2]) };
		return g_arrInterface->CreateArray(data, 3, scriptObj);
	}

	void AddVector(std::vector<const char*>& keys, std::vector<Element>& values, const char* key,
		const float* v, Script* scriptObj)
	{
		if (Array* arr = MakeVector(v, scriptObj))
			AddKey(keys, values, key, Element(arr));
	}

	Array* MakeMap(std::vector<const char*>& keys, std::vector<Element>& values, Script* scriptObj)
	{
		if (keys.empty())
			return g_arrInterface->CreateStringMap(nullptr, nullptr, 0, scriptObj);
		return g_arrInterface->CreateStringMap(const_cast<const char**>(keys.data()), values.data(),
			(UInt32)keys.size(), scriptObj);
	}

	//one sub-map per edge of the triangle that actually carries cover
	Array* BuildEdges(void* navMesh, UInt16 triIndex, void* tri, Script* scriptObj)
	{
		Array* edges = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
		if (!edges)
			return nullptr;

		for (UInt32 slot = 0; slot < 2; ++slot)
		{
			UInt16 bucket = 0;
			bool leftOpen = false, rightOpen = false;
			ThisCall<void>(0x691040, tri, slot, &bucket, &leftOpen, &rightOpen); //GetEdgeCoverData

			//bucket 1 is "wall with no cover", the engine treats >= 2 or either open flag as usable
			if (!(bucket >= 2 || leftOpen || rightOpen))
				continue;

			EdgeEndpoints edge = {};
			ThisCall<void>(0x68F040, navMesh, &edge, triIndex, slot); //NavMesh::GetEdgeEndpoints
			if (!edge.p0 || !edge.p1)
				continue;

			std::vector<const char*> keys;
			std::vector<Element> values;
			AddKey(keys, values, "slot", Element((double)slot));
			AddKey(keys, values, "heightBucket", Element((double)bucket));
			AddKey(keys, values, "leftOpen", Element((double)leftOpen));
			AddKey(keys, values, "rightOpen", Element((double)rightOpen));
			AddVector(keys, values, "edgeA", &edge.p0->x, scriptObj);
			AddVector(keys, values, "edgeB", &edge.p1->x, scriptObj);

			if (Array* entry = MakeMap(keys, values, scriptObj))
				g_arrInterface->AppendElement(edges, Element(entry));
		}

		return edges;
	}

	//the AI's reserved cover spot, which carries its own triangle and pre-classified edge
	Array* BuildAICover(void* coverLocation, CombatProcedureBeInCoverView* proc, Script* scriptObj)
	{
		auto* cover = &reinterpret_cast<CombatCoverLocationLayout*>(coverLocation)->cover;

		std::vector<const char*> keys;
		std::vector<Element> values;

		ScopedNavMeshPtr navMesh;
		UInt16 triangle = 0xFFFF;
		if (ThisCall<bool>(0x6DD640, &cover->location, navMesh.Slot(), &triangle))
		{
			AddKey(keys, values, "triangle", Element((double)triangle));
			if (auto* navForm = static_cast<TESForm*>(navMesh.Get()))
				AddKey(keys, values, "navmesh", Element(navForm));
		}

		AddKey(keys, values, "edgeSlot", Element((double)cover->edgeSlot));
		AddKey(keys, values, "heightBucket", Element((double)cover->heightBucket));
		AddKey(keys, values, "crouchCover", Element((double)(cover->crouchCover != 0)));
		AddKey(keys, values, "standCover", Element((double)(cover->standCover != 0)));
		AddKey(keys, values, "leftOpen", Element((double)(cover->leftOpen != 0)));
		AddKey(keys, values, "rightOpen", Element((double)(cover->rightOpen != 0)));
		AddVector(keys, values, "edgeA", &cover->edgeA.x, scriptObj);
		AddVector(keys, values, "edgeB", &cover->edgeB.x, scriptObj);
		AddVector(keys, values, "coverNormal", &cover->coverNormal.x, scriptObj);
		AddVector(keys, values, "position",
			&reinterpret_cast<CombatCoverLocationLayout*>(coverLocation)->position.x, scriptObj);

		if (proc)
		{
			AddVector(keys, values, "coverPos", proc->coverPos, scriptObj);
			AddVector(keys, values, "firePos", proc->firePos, scriptObj);
		}

		return MakeMap(keys, values, scriptObj);
	}
}

DEFINE_COMMAND_PLUGIN(GetActorCoverState, "Returns the actor's cover procedure state (0 INITIALIZING, 1 WAITING_BEHIND_COVER, 2 MOVING_OUT, 3 WAITING_OUT_OF_COVER, 4 FIRING_OUT_OF_COVER, 5 MOVING_IN, 6 MOVING_IN_AND_ROTATE, 7 HOLDING_GROUND), or -1 when no from-cover action is running", 1, 0, nullptr);
DEFINE_COMMAND_PLUGIN(GetActorCoverInfo, "Returns a string map for the navmesh triangle the actor is standing on: triangle, navmesh, hasCover, edges (per cover edge: slot, heightBucket, leftOpen, rightOpen, edgeA, edgeB). Adds state, stateName, hasProcedure and a cover sub-map when the combat AI has cover reserved. Returns 0 only when the actor is off the navmesh with no reserved cover", 1, 0, nullptr);

bool Cmd_GetActorCoverState_Execute(COMMAND_ARGS)
{
	*result = -1;

	if (!IsActorRef(thisObj))
		return true;

	void* controller = Engine::Actor_GetCombatController(static_cast<Actor*>(thisObj));
	auto* proc = reinterpret_cast<CombatProcedureBeInCoverView*>(FindCoverProcedure(controller));
	if (!proc)
		return true;

	*result = (double)proc->coverState;
	return true;
}

bool Cmd_GetActorCoverInfo_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!g_arrInterface || !IsActorRef(thisObj))
		return true;

	ScopedNavMeshPtr navMesh;
	UInt16 triangle = 0xFFFF;
	const bool onNavmesh = ResolveActorTriangle(thisObj, navMesh, triangle);

	void* controller = Engine::Actor_GetCombatController(static_cast<Actor*>(thisObj));
	auto* proc = reinterpret_cast<CombatProcedureBeInCoverView*>(FindCoverProcedure(controller));
	void* coverLocation = proc && proc->coverLocation ? proc->coverLocation : GetReservedCoverLocation(controller);

	if (!onNavmesh && !coverLocation)
		return true;

	std::vector<const char*> keys;
	std::vector<Element> values;

	if (onNavmesh)
	{
		AddKey(keys, values, "triangle", Element((double)triangle));
		if (auto* navForm = static_cast<TESForm*>(navMesh.Get()))
			AddKey(keys, values, "navmesh", Element(navForm));

		void* tri = GetTriangle(navMesh.Get(), triangle);
		const bool usable = tri && !ThisCall<bool>(0x691140, tri, kTriangleFlag_Disabled); //IsFlagSet
		const bool hasCover = usable && ThisCall<bool>(0x690770, tri); //NavMeshTriangle::HasCover
		AddKey(keys, values, "hasCover", Element((double)hasCover));
		if (hasCover)
		{
			if (Array* edges = BuildEdges(navMesh.Get(), triangle, tri, scriptObj))
				AddKey(keys, values, "edges", Element(edges));
		}
	}

	const SInt32 state = proc ? (SInt32)proc->coverState : -1;
	AddKey(keys, values, "state", Element((double)state));
	if (state >= 0 && state < (SInt32)kCoverState_Count)
		AddKey(keys, values, "stateName", Element(kCoverStateNames[state]));
	AddKey(keys, values, "hasProcedure", Element((double)(proc != nullptr)));

	if (coverLocation)
	{
		if (Array* aiCover = BuildAICover(coverLocation, proc, scriptObj))
			AddKey(keys, values, "cover", Element(aiCover));
	}

	Array* map = MakeMap(keys, values, scriptObj);
	if (!map)
		return true;

	g_arrInterface->AssignCommandResult(map, result);
	return true;
}

namespace CoverCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_GetActorCoverState);
		nvse->RegisterTypedCommand(&kCommandInfo_GetActorCoverInfo, kRetnType_Array);
	}
}
