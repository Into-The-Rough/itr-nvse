//polls bhkCharacterController state each frame to detect jumps and landings
//no vtable hooks - avoids shared-slot recursion with other plugins

#include <vector>
#include <algorithm>
#include <cstdint>

#include "OnJumpLandHandler.h"
#define ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/NVSEMinimal.h"
#undef ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"
#include "internal/HavokLayout.h"

enum HkCharState : UInt32 {
	kHkState_OnGround = 0,
	kHkState_Jumping = 1,
	kHkState_InAir = 2,
	kHkState_Climbing = 3,
	kHkState_Flying = 4,
	kHkState_Swimming = 5,
	kHkState_Projectile = 6,
};

struct TrackedEntry {
	UInt32 refID;
	UInt32 state;
	float fallTime;
	UInt32 lastSeenFrame;
};

struct PendingEvent {
	UInt8 type; //1=jump, 2=land
	UInt32 refID;
	float fallTime;
};

static constexpr const char* kJumpStartEvent = "ITR:OnJumpStart";
static constexpr const char* kActorLandedEvent = "ITR:OnActorLanded";
static constexpr int kProbePriority = -9999;
static constexpr UInt32 kListenerProbeIntervalFrames = 30;

static std::vector<TrackedEntry> s_tracked;
static std::vector<PendingEvent> s_pending;
static UInt32 s_frameTag = 0;
static UInt32 s_listenerProbeFrame = kListenerProbeIntervalFrames;
static PluginHandle s_pluginHandle = kPluginHandle_Invalid;
static bool s_probeHandlersInstalled = false;
static bool s_hasJumpListeners = true;
static bool s_hasLandListeners = true;
static bool s_reserved = false;
static bool s_inUpdate = false;

static void JumpProbeHandler(TESObjectREFR*, void*) {}
static void LandProbeHandler(TESObjectREFR*, void*) {}

struct UpdateGuard {
	UpdateGuard() { s_inUpdate = true; }
	~UpdateGuard() { s_inUpdate = false; }
};

static UInt32 ReadState(const BhkCharacterControllerView* charCtrl) {
	return charCtrl ? charCtrl->chrContext.hkState : 0xFFFFFFFF;
}

static float ReadFallTime(const BhkCharacterControllerView* charCtrl) {
	return charCtrl ? charCtrl->fallTime : 0.0f;
}

static BhkCharacterControllerView* GetCharController(Actor* actor) {
	if (!actor) return nullptr;

	if (actor->typeID != kFormType_ACHR && actor->typeID != kFormType_ACRE)
		return nullptr;

	BaseProcess* process = actor->baseProcess;
	if (!process) return nullptr;

	if (process->processLevel > 1) return nullptr;

	return reinterpret_cast<BhkCharacterControllerView*>(
		reinterpret_cast<ProcessControllerView*>(process)->characterController);
}

static void EnsureReserved() {
	if (s_reserved) return;
	s_tracked.reserve(128);
	s_pending.reserve(128);
	s_reserved = true;
}

static std::vector<TrackedEntry>::iterator FindTracked(UInt32 refID) {
	return std::lower_bound(s_tracked.begin(), s_tracked.end(), refID,
		[](const TrackedEntry& entry, UInt32 value) {
			return entry.refID < value;
		});
}

static void QueueEdgeEvents(UInt32 refID, UInt32 prevState, float prevFallTime, UInt32 state) {
	if (s_hasJumpListeners && state == kHkState_Jumping && prevState != kHkState_Jumping)
		s_pending.push_back({1, refID, 0.0f});

	bool wasAirborne = (prevState == kHkState_Jumping || prevState == kHkState_InAir);
	// mirrors old hook: newState == 0 || (newState & 0x2) == 0
	bool nowGrounded = (state == 0 || (state & 0x2) == 0);
	if (s_hasLandListeners && wasAirborne && nowGrounded && state != kHkState_Jumping)
		s_pending.push_back({2, refID, prevFallTime});
}

static void ProcessActor(Actor* actor) {
	if (!actor) return;
	BhkCharacterControllerView* ctrl = GetCharController(actor);
	if (!ctrl) return;

	UInt32 refID = actor->refID;
	if (!refID) return;

	auto it = FindTracked(refID);
	if (it != s_tracked.end() && it->refID == refID && it->lastSeenFrame == s_frameTag)
		return;

	UInt32 state = ReadState(ctrl);
	if (state == 0xFFFFFFFF) return;

	float fallTime = ReadFallTime(ctrl);

	if (it != s_tracked.end() && it->refID == refID) {
		QueueEdgeEvents(refID, it->state, it->fallTime, state);
		it->state = state;
		it->fallTime = fallTime;
		it->lastSeenFrame = s_frameTag;
		return;
	}

	s_tracked.insert(it, {refID, state, fallTime, s_frameTag});
}

static void ProcessActorsFromProcessManager() {
	auto* processManager = reinterpret_cast<ProcessManagerLite*>(g_processManager);
	if (!processManager || !processManager->objects.data) return;

	UInt32 upperBound = processManager->objects.firstFreeEntry;

	for (int bucket = 0; bucket < 2; bucket++) {
		UInt32 begin = processManager->beginOffsets[bucket];
		UInt32 end = processManager->endOffsets[bucket];
		if (begin > upperBound) begin = upperBound;
		if (end > upperBound) end = upperBound;

		auto** objArray = processManager->objects.data + begin;
		auto** arrEnd = processManager->objects.data + end;
		for (; objArray < arrEnd; ++objArray)
			ProcessActor(reinterpret_cast<Actor*>(*objArray));
	}

	if (*g_thePlayerPtr)
		ProcessActor(static_cast<Actor*>(*g_thePlayerPtr));
}

static bool InstallProbeHandler(const char* eventName, NVSEEventManagerInterface::NativeEventHandler handler,
	const char* handlerName)
{
	if (!g_eventManagerInterface ||
		!g_eventManagerInterface->SetNativeEventHandlerWithPriority ||
		!g_eventManagerInterface->RemoveNativeEventHandlerWithPriority ||
		s_pluginHandle == kPluginHandle_Invalid)
	{
		return false;
	}

	g_eventManagerInterface->RemoveNativeEventHandlerWithPriority(eventName, handler, kProbePriority);
	return g_eventManagerInterface->SetNativeEventHandlerWithPriority(
		eventName, handler, s_pluginHandle, handlerName, kProbePriority);
}

static bool HasExternalHandlers(const char* eventName, NVSEEventManagerInterface::NativeEventHandler probeHandler) {
	if (!s_probeHandlersInstalled || !g_eventManagerInterface || !g_eventManagerInterface->IsEventHandlerFirst)
		return true;

	return !g_eventManagerInterface->IsEventHandlerFirst(
		eventName, probeHandler, kProbePriority,
		nullptr, 0, nullptr, 0, nullptr, 0);
}

static void RefreshListenerState(bool force) {
	if (!s_probeHandlersInstalled)
		OnJumpLandHandler::InstallListenerProbes();

	if (!s_probeHandlersInstalled) {
		s_hasJumpListeners = true;
		s_hasLandListeners = true;
		return;
	}

	if (!force && s_listenerProbeFrame++ < kListenerProbeIntervalFrames)
		return;

	s_listenerProbeFrame = 0;
	s_hasJumpListeners = HasExternalHandlers(kJumpStartEvent, JumpProbeHandler);
	s_hasLandListeners = HasExternalHandlers(kActorLandedEvent, LandProbeHandler);
}

static void AdvanceFrameTag() {
	if (++s_frameTag == 0) {
		s_tracked.clear();
		s_frameTag = 1;
	}
}

static void DispatchPendingEvents() {
	for (const auto& evt : s_pending) {
		auto* actor = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(evt.refID));
		if (!actor) continue;

		if (evt.type == 1)
			g_eventManagerInterface->DispatchEvent(kJumpStartEvent,
				actor, static_cast<TESForm*>(actor));
		else
			g_eventManagerInterface->DispatchEvent(kActorLandedEvent,
				actor, static_cast<TESForm*>(actor), PackEventFloatArg(evt.fallTime));
	}

	s_pending.clear();
}

namespace OnJumpLandHandler {
void InstallListenerProbes()
{
	bool jumpProbe = InstallProbeHandler(kJumpStartEvent, JumpProbeHandler, "ITR_OnJumpStartProbe");
	bool landProbe = InstallProbeHandler(kActorLandedEvent, LandProbeHandler, "ITR_OnActorLandedProbe");

	s_probeHandlersInstalled = jumpProbe && landProbe;
	s_listenerProbeFrame = kListenerProbeIntervalFrames;
	s_hasJumpListeners = true;
	s_hasLandListeners = true;
}

void ClearState()
{
	s_tracked.clear();
	s_pending.clear();
	s_probeHandlersInstalled = false;
	s_listenerProbeFrame = kListenerProbeIntervalFrames;
	s_hasJumpListeners = true;
	s_hasLandListeners = true;
}

void Update()
{
	if (!g_eventManagerInterface) return;
	if (s_inUpdate) return;
	UpdateGuard guard;

	RefreshListenerState(false);
	if (!s_hasJumpListeners && !s_hasLandListeners) {
		if (!s_tracked.empty())
			s_tracked.clear();
		return;
	}

	EnsureReserved();
	s_pending.clear();
	AdvanceFrameTag();
	ProcessActorsFromProcessManager();

	s_tracked.erase(std::remove_if(s_tracked.begin(), s_tracked.end(),
		[](const TrackedEntry& entry) {
			return entry.lastSeenFrame != s_frameTag;
		}), s_tracked.end());

	DispatchPendingEvents();
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;
	s_pluginHandle = nvse->GetPluginHandle ? nvse->GetPluginHandle() : kPluginHandle_Invalid;
	return true;
}
}
