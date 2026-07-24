//swaps the get-up FSM vtable slot (+0x41C, both process vtables) to observe
//knockedState transitions at process+0x13C and fire ITR:OnKnockdown / ITR:OnGetUp

#include <vector>
#include <Windows.h>

#include "OnKnockdownHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/GameSDK.h"
#include "internal/globals.h"

//get-up FSM per-frame update, sub_920150, installed in both process vtables
constexpr UInt32 kVtbl_HighProcess = 0x1087864;
constexpr UInt32 kVtbl_MiddleHighProcess = 0x108904C;
constexpr UInt32 kFSMSlotOffset = 0x41C;

//knockedState values at process+0x13C
constexpr SInt8 kState_Upright = 0;
constexpr SInt8 kState_RagdollImpulse = 2;
constexpr SInt8 kState_Paralysis = 3;
constexpr SInt8 kState_RagdollHavok = 4;
constexpr SInt8 kState_GetUpAnim = 5;
constexpr SInt8 kState_GetUpFinal = 6;

//ITR:OnKnockdown cause values
constexpr int kCauseForce = 1;
constexpr int kCauseParalysis = 2;
constexpr int kCauseHavok = 3;

//ITR:OnGetUp phase values
constexpr int kPhaseBegin = 0;
constexpr int kPhaseComplete = 1;

struct QueuedKnockEvent {
	UInt32 actorRefID;
	bool isGetUp;
	int code; //cause for knockdown, phase for get-up
};

namespace OnKnockdownHandler {
	std::vector<QueuedKnockEvent> g_pendingEvents;
	CRITICAL_SECTION g_stateLock;
	volatile LONG g_stateLockInit = 0;
	DWORD g_mainThreadId = 0;
}

static void EnsureStateLockInitialized()
{
	InitCriticalSectionOnce(&OnKnockdownHandler::g_stateLockInit, &OnKnockdownHandler::g_stateLock);
}

typedef void(__thiscall* KnockdownFSM_t)(void* process, TESObjectREFR* actor);
static KnockdownFSM_t s_origHigh = nullptr;
static KnockdownFSM_t s_origMiddleHigh = nullptr;

//per-process last-seen knockedState, keyed by process pointer. detects external
//writes (PushActorAway 0->2) via prev->before and FSM-internal writes via before->after.
//actorRefID guards against process allocator reuse handing the address to a new actor,
//lastTouchFrame lets Update() sweep entries whose actor unloaded while not upright
struct ProcessStateEntry {
	void* process;
	UInt32 actorRefID;
	UInt32 lastTouchFrame;
	SInt8 lastState;
};
static ProcessStateEntry s_stateCache[64] = {0};
static volatile UInt32 s_frameCounter = 0;
constexpr UInt32 kCacheStaleFrames = 600; //knocked actors tick every frame, 10s idle means gone

static SInt8 ReadKnockedState(void* process)
{
	return *((SInt8*)process + 0x13C); //knockedState
}

static ProcessStateEntry* FindCacheEntry(void* process)
{
	for (ProcessStateEntry& e : s_stateCache)
		if (e.process == process) return &e;
	return nullptr;
}

//when full, replaces the stalest entry rather than dropping the new actor
static ProcessStateEntry* AllocCacheEntry(void* process, bool* evicted)
{
	*evicted = false;
	ProcessStateEntry* stalest = nullptr;
	for (ProcessStateEntry& e : s_stateCache) {
		if (!e.process) { e.process = process; return &e; }
		if (!stalest || e.lastTouchFrame < stalest->lastTouchFrame) stalest = &e;
	}
	if (stalest) { stalest->process = process; *evicted = true; }
	return stalest;
}

static void KnockdownProbe(TESObjectREFR*, void*) {}
static void GetUpProbe(TESObjectREFR*, void*) {}
static EventDispatch::ListenerProbe s_probeKnockdown = { "ITR:OnKnockdown", "ITR_OnKnockdownProbe", KnockdownProbe };
static EventDispatch::ListenerProbe s_probeGetUp = { "ITR:OnGetUp", "ITR_OnGetUpProbe", GetUpProbe };

static void QueueEvent(UInt32 actorRefID, bool isGetUp, int code)
{
	constexpr size_t kMaxQueuedEvents = 256;
	ScopedLock lock(&OnKnockdownHandler::g_stateLock);
	if (!g_eventManagerInterface) return;
	if (OnKnockdownHandler::g_pendingEvents.size() >= kMaxQueuedEvents) return;
	OnKnockdownHandler::g_pendingEvents.push_back({actorRefID, isGetUp, code});
}

static void ObserveTransition(void* process, TESObjectREFR* actor, SInt8 before, SInt8 after)
{
	if (OnKnockdownHandler::g_stateLockInit != 2) return;

	UInt32 actorRefID = actor ? ((TESForm*)actor)->refID : 0;
	if (!actorRefID) return;

	SInt8 prev;
	bool fresh = false;
	bool continuing = false;
	{
		ScopedLock lock(&OnKnockdownHandler::g_stateLock);
		ProcessStateEntry* entry = FindCacheEntry(process);
		if (!entry) {
			bool evicted = false;
			entry = AllocCacheEntry(process, &evicted);
			fresh = true;
			//a full-cache eviction can throw out a still-ragdolled actor, whose next tick
			//re-creates it as fresh at a ragdoll state and would re-fire the knockdown.
			//first sight already mid-ragdoll on a reused slot counts as continuing
			continuing = evicted &&
				(before == kState_RagdollImpulse || before == kState_Paralysis || before == kState_RagdollHavok);
		}
		else if (entry->actorRefID != actorRefID) fresh = true; //process address recycled for a new actor
		if (entry) {
			if (fresh) entry->actorRefID = actorRefID;
			entry->lastTouchFrame = s_frameCounter;
			prev = fresh ? before : entry->lastState;
			if (after == kState_Upright) { entry->process = nullptr; entry->actorRefID = 0; entry->lastState = 0; }
			else entry->lastState = after;
		}
		else prev = before;
	}

	//force knockdown: PushActorAway writes state 2 outside the FSM. upright actors aren't cached,
	//so a fresh entry at state 2 is a new knockdown (prev seeds to before and would mask it)
	if (before == kState_RagdollImpulse && !continuing && (fresh || prev != kState_RagdollImpulse)) {
		if (s_probeKnockdown.hasListeners) QueueEvent(actorRefID, false, kCauseForce);
	}

	//FSM-internal writes, seen as before->after
	if (before == kState_Upright && after == kState_Paralysis) {
		if (s_probeKnockdown.hasListeners) QueueEvent(actorRefID, false, kCauseParalysis);
	}
	else if (before == kState_Upright && after == kState_RagdollHavok) {
		if (s_probeKnockdown.hasListeners) QueueEvent(actorRefID, false, kCauseHavok);
	}
	else if (after == kState_GetUpAnim && before != kState_GetUpAnim && before != kState_GetUpFinal) {
		if (s_probeGetUp.hasListeners) QueueEvent(actorRefID, true, kPhaseBegin);
	}
	else if (before == kState_GetUpFinal && after == kState_Upright) {
		if (s_probeGetUp.hasListeners) QueueEvent(actorRefID, true, kPhaseComplete);
	}
}

static void __fastcall Hook_KnockdownFSM(void* process, void*, TESObjectREFR* actor)
{
	UInt32 vtbl = *(UInt32*)process;
	KnockdownFSM_t orig = (vtbl == kVtbl_HighProcess) ? s_origHigh : s_origMiddleHigh;

	if (!s_probeKnockdown.hasListeners && !s_probeGetUp.hasListeners) {
		if (orig) orig(process, actor);
		return;
	}

	SInt8 before = ReadKnockedState(process);
	if (orig) orig(process, actor);
	SInt8 after = ReadKnockedState(process);

	if (before != after || after == kState_RagdollImpulse)
		ObserveTransition(process, actor, before, after);
}

namespace OnKnockdownHandler {
void InstallListenerProbe()
{
	s_probeKnockdown.Install();
	s_probeGetUp.Install();
}

void Update()
{
	if (OnKnockdownHandler::g_stateLockInit != 2) return;

	s_probeKnockdown.Refresh(false);
	s_probeGetUp.Refresh(false);

	DWORD currentThreadId = GetCurrentThreadId();
	if (!OnKnockdownHandler::g_mainThreadId)
		OnKnockdownHandler::g_mainThreadId = currentThreadId;
	if (currentThreadId != OnKnockdownHandler::g_mainThreadId)
		return;

	s_frameCounter++;

	std::vector<QueuedKnockEvent> queuedEvents;
	{
		ScopedLock lock(&OnKnockdownHandler::g_stateLock);
		queuedEvents.swap(OnKnockdownHandler::g_pendingEvents);

		//sweep entries whose actor unloaded/died while not upright, they never
		//see the upright transition that normally evicts them
		if ((s_frameCounter & 63) == 0) {
			for (ProcessStateEntry& e : s_stateCache) {
				if (e.process && s_frameCounter - e.lastTouchFrame > kCacheStaleFrames) {
					e.process = nullptr;
					e.actorRefID = 0;
					e.lastState = 0;
				}
			}
		}
	}

	for (const QueuedKnockEvent& evt : queuedEvents) {
		Actor* actor = reinterpret_cast<Actor*>(Engine::LookupFormByID(evt.actorRefID));
		if (!actor) continue;
		if (!g_eventManagerInterface) break;

		if (evt.isGetUp) {
			Log("OnKnockdownHandler: OnGetUp actor=%08X phase=%d", evt.actorRefID, evt.code);
			g_eventManagerInterface->DispatchEvent("ITR:OnGetUp",
				reinterpret_cast<TESObjectREFR*>(actor), actor, evt.code);
		}
		else {
			Log("OnKnockdownHandler: OnKnockdown actor=%08X cause=%d", evt.actorRefID, evt.code);
			g_eventManagerInterface->DispatchEvent("ITR:OnKnockdown",
				reinterpret_cast<TESObjectREFR*>(actor), actor, evt.code);
		}
	}
}

void ClearState()
{
	if (OnKnockdownHandler::g_stateLockInit != 2) return;
	ScopedLock lock(&OnKnockdownHandler::g_stateLock);
	OnKnockdownHandler::g_pendingEvents.clear();
	for (ProcessStateEntry& e : s_stateCache) { e.process = nullptr; e.actorRefID = 0; e.lastTouchFrame = 0; e.lastState = 0; }
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	EnsureStateLockInitialized();
	OnKnockdownHandler::g_mainThreadId = GetCurrentThreadId();

	//chain-tolerant: capture whatever is currently in the slot as our original
	s_origHigh = (KnockdownFSM_t)*(UInt32*)(kVtbl_HighProcess + kFSMSlotOffset);
	s_origMiddleHigh = (KnockdownFSM_t)*(UInt32*)(kVtbl_MiddleHighProcess + kFSMSlotOffset);
	if (!s_origHigh || !s_origMiddleHigh) return false;

	SafeWrite::Write32(kVtbl_HighProcess + kFSMSlotOffset, (UInt32)&Hook_KnockdownFSM);
	SafeWrite::Write32(kVtbl_MiddleHighProcess + kFSMSlotOffset, (UInt32)&Hook_KnockdownFSM);

	Log("OnKnockdownHandler: FSM slot swapped, originals High=%08X MiddleHigh=%08X",
		(UInt32)s_origHigh, (UInt32)s_origMiddleHigh);
	return true;
}
}
