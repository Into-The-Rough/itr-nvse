//polls bhkCharacterController state each frame to detect jumps and landings
//no vtable hooks - avoids shared-slot recursion with other plugins

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

#include "OnJumpLandHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"

enum HkCharState : UInt32 {
	kHkState_OnGround = 0,
	kHkState_Jumping = 1,
	kHkState_InAir = 2,
	kHkState_Climbing = 3,
	kHkState_Flying = 4,
	kHkState_Swimming = 5,
	kHkState_Projectile = 6,
};

static void* g_processManager = (void*)0x11E0E80;
static void** g_thePlayer = (void**)0x011DEA3C;

template <typename T>
struct NiTArrayLite {
	void** vtbl;
	T* data;
	UInt16 capacity;
	UInt16 firstFreeEntry;
	UInt16 numObjs;
	UInt16 growSize;
};

struct ProcessManagerLite {
	UInt32 unk000;
	NiTArrayLite<void*> objects;
	UInt32 beginOffsets[4];
	UInt32 endOffsets[4];
};

struct SampledActor {
	void* actor;
	UInt32 refID;
	UInt32 state;
	float fallTime;
};

struct TrackedState {
	UInt32 state;
	float fallTime;
};

static std::unordered_map<UInt32, TrackedState> g_trackedStates;
static bool g_initialized = false;

static UInt32 ReadState(const void* charCtrl) {
	return charCtrl ? *(const UInt32*)((const UInt8*)charCtrl + 0x3F0) : 0xFFFFFFFF;
}

static float ReadFallTime(const void* charCtrl) {
	return charCtrl ? *(const float*)((const UInt8*)charCtrl + 0x548) : 0.0f;
}

static void* GetCharController(void* actor) {
	if (!actor) return nullptr;

	UInt8 typeID = *((UInt8*)actor + 4);
	if (typeID != 0x3B && typeID != 0x3C) return nullptr;

	void* process = *(void**)((UInt8*)actor + 0x68);
	if (!process) return nullptr;

	UInt32 processLevel = *(UInt32*)((UInt8*)process + 0x28);
	if (processLevel > 1) return nullptr;

	return *(void**)((UInt8*)process + 0x138);
}

static void TryAddActor(std::vector<SampledActor>& out, std::unordered_set<UInt32>& seenRefIDs, void* actor) {
	if (!actor) return;
	void* ctrl = GetCharController(actor);
	if (!ctrl) return;
	UInt32 refID = *(UInt32*)((UInt8*)actor + 0x0C);
	if (!refID || !seenRefIDs.insert(refID).second) return;

	UInt32 state = ReadState(ctrl);
	if (state == 0xFFFFFFFF) return;

	out.push_back({actor, refID, state, ReadFallTime(ctrl)});
}

static void CollectFromProcessManager(std::vector<SampledActor>& out) {
	auto* processManager = reinterpret_cast<ProcessManagerLite*>(g_processManager);
	if (!processManager || !processManager->objects.data) return;

	std::unordered_set<UInt32> seenRefIDs;
	seenRefIDs.reserve(64);

	UInt32 upperBound = processManager->objects.firstFreeEntry;

	for (int bucket = 0; bucket < 2; bucket++) {
		UInt32 begin = processManager->beginOffsets[bucket];
		UInt32 end = processManager->endOffsets[bucket];
		if (begin > upperBound) begin = upperBound;
		if (end > upperBound) end = upperBound;

		auto** objArray = processManager->objects.data + begin;
		auto** arrEnd = processManager->objects.data + end;
		for (; objArray != arrEnd; ++objArray)
			TryAddActor(out, seenRefIDs, *objArray);
	}

	void* player = g_thePlayer ? *g_thePlayer : nullptr;
	if (player)
		TryAddActor(out, seenRefIDs, player);
}

struct PendingEvent {
	UInt8 type; //1=jump, 2=land
	UInt32 refID;
	float fallTime;
};

namespace OnJumpLandHandler {
void Update()
{
	if (!g_eventManagerInterface) return;

	std::vector<SampledActor> currentActors;
	currentActors.reserve(g_trackedStates.size() + 8);
	CollectFromProcessManager(currentActors);

	std::vector<PendingEvent> pending;
	pending.reserve(currentActors.size());

	std::unordered_map<UInt32, TrackedState> nextTrackedStates;
	nextTrackedStates.reserve(currentActors.size());

	for (const auto& actor : currentActors) {
		UInt32 prevState = actor.state;
		float prevFallTime = actor.fallTime;

		auto prevIt = g_trackedStates.find(actor.refID);
		if (prevIt != g_trackedStates.end()) {
			prevState = prevIt->second.state;
			prevFallTime = prevIt->second.fallTime;
		}

		if (actor.state == kHkState_Jumping && prevState != kHkState_Jumping)
			pending.push_back({1, actor.refID, 0.0f});

		bool wasAirborne = (prevState == kHkState_Jumping || prevState == kHkState_InAir);
		bool nowGrounded = (actor.state == 0 || (actor.state & 0x2) == 0);
		if (wasAirborne && nowGrounded && actor.state != kHkState_Jumping)
			pending.push_back({2, actor.refID, prevFallTime});

		nextTrackedStates.emplace(actor.refID, TrackedState{actor.state, actor.fallTime});
	}

	g_trackedStates.swap(nextTrackedStates);

	for (const auto& evt : pending) {
		void* actor = Engine::LookupFormByID(evt.refID);
		if (!actor) continue;

		if (evt.type == 1)
			g_eventManagerInterface->DispatchEvent("ITR:OnJumpStart",
				reinterpret_cast<TESObjectREFR*>(actor), (TESForm*)actor);
		else
			g_eventManagerInterface->DispatchEvent("ITR:OnActorLanded",
				reinterpret_cast<TESObjectREFR*>(actor),
				(TESForm*)actor, PackEventFloatArg(evt.fallTime));
	}
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;
	g_initialized = true;
	return true;
}
}
