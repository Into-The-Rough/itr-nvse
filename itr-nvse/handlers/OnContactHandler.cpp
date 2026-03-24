//3-channel contact event system
//
//channel 0: rigid body contacts (FOCollisionListener::contactPointAddedCallback)
//channel 1: character proxy contacts (hkpCharacterProxy::fireContactAdded/Removed)
//channel 2: phantom overlaps (hkpSimpleShapePhantom/hkpCachingShapePhantom add/remove)
//
//all havok callbacks fire during bhkWorld::Update which can run multithreaded.
//events are queued under CRITICAL_SECTION and dispatched on main thread in Update().
//
//pair coalescing: multiple contact points between the same two refs produce one
//begin/end event. g_activeContacts tracks refcount per pair, fires on 0->1 / 1->0.

#include "OnContactHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/ScopedLock.h"
#include "internal/Detours.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>

extern void Log(const char* fmt, ...);

enum ContactChannel : UInt8 {
	kChannel_RigidBody = 0,
	kChannel_CharProxy = 1,
	kChannel_Phantom = 2,
};

struct QueuedContactEvent {
	UInt32 watchedRefID;
	UInt32 otherRefID;
	UInt8 channel;
	bool isBegin;
};

struct ContactPair {
	UInt32 refA;
	UInt32 refB;
	UInt8 channel;
	bool operator==(const ContactPair& o) const {
		return refA == o.refA && refB == o.refB && channel == o.channel;
	}
};

struct ContactPairHash {
	size_t operator()(const ContactPair& p) const {
		size_t h = p.refA;
		h ^= p.refB + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= p.channel + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

//all shared state protected by g_contactLock
//hooks read g_watchedSnapshot (copied from g_watchedRefIDs on main thread)
static std::unordered_set<UInt32> g_watchedRefIDs;         //main thread only
static std::unordered_set<UInt32> g_watchedSnapshot;       //read by hooks under lock
static std::unordered_map<ContactPair, int, ContactPairHash> g_activeContacts;
static std::vector<QueuedContactEvent> g_pendingEvents;
static CRITICAL_SECTION g_contactLock;
static volatile LONG g_lockInit = 0;
static DWORD g_mainThreadId = 0;
static bool g_snapshotDirty = true;

static constexpr size_t kMaxQueuedEvents = 2048;

//ref resolution: hkpCollidable -> TESObjectREFR*
//same chain FOCollisionListener uses:
//  getRoot -> hkpGetRigidBody -> GetbhkCollisionObject -> NiNode(+0x08) -> FindReferenceFor3D
typedef void* (__thiscall *_getRoot)(void*);
typedef void* (__cdecl *_hkpGetRigidBody)(void* collidable);
typedef void* (__cdecl *_GetbhkCollisionObject)(void* worldObj);
typedef void* (__cdecl *_FindReferenceFor3D)(void* niNode);

static const _getRoot getRoot = (_getRoot)0x624020;
static const _hkpGetRigidBody hkpGetRigidBody = (_hkpGetRigidBody)0x4B59F0;
static const _GetbhkCollisionObject GetbhkCollisionObject = (_GetbhkCollisionObject)0x4B5A20;
static const _FindReferenceFor3D FindReferenceFor3D = (_FindReferenceFor3D)0x56F930;

static UInt32 ResolveCollidableToRefID(void* collidable) {
	if (!collidable) return 0;

	void* root = getRoot(collidable);
	if (!root) return 0;

	void* rigidBody = hkpGetRigidBody(root);
	if (!rigidBody) return 0;

	void* collObj = GetbhkCollisionObject(rigidBody);
	if (!collObj) return 0;

	//bhkCollisionObject+0x08 (verified from FOCollisionListener disasm)
	void* niNode = *(void**)((UInt8*)collObj + 0x08);
	if (!niNode) return 0;

	void* refr = FindReferenceFor3D(niNode);
	if (!refr) return 0;

	return *(UInt32*)((UInt8*)refr + 0x0C); //refID
}

static void QueueContactEvent(UInt32 watchedRefID, UInt32 otherRefID, UInt8 channel, bool isBegin) {
	if (g_lockInit != 2) return;
	ScopedLock lock(&g_contactLock);
	if (g_pendingEvents.size() >= kMaxQueuedEvents) {
		Log("OnContactHandler: queue full (%u events), dropping %s for 0x%08X",
			(UInt32)g_pendingEvents.size(), isBegin ? "begin" : "end", watchedRefID);
		return;
	}
	g_pendingEvents.push_back({watchedRefID, otherRefID, channel, isBegin});
}

//--- channel 0: rigid body contacts ---

static Detours::JumpDetour s_rigidBodyDetour;
typedef void (__thiscall *_FOCollisionListener_contactPointAdded)(void*, void*);

static void __fastcall Hook_ContactPointAdded(void* listener, void* edx, void* event) {
	//call original first
	s_rigidBodyDetour.GetTrampoline<_FOCollisionListener_contactPointAdded>()(listener, event);

	//read snapshot under lock - safe from any thread
	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;

	//hkpContactPointAddedEvent: m_bodyA at +0x00, m_bodyB at +0x04
	//verified from FOCollisionListener disasm
	void* bodyA = *(void**)((UInt8*)event + 0x00);
	void* bodyB = *(void**)((UInt8*)event + 0x04);

	UInt32 refA = ResolveCollidableToRefID(bodyA);
	UInt32 refB = ResolveCollidableToRefID(bodyB);

	//queue under same lock (already held)
	if (refA && g_watchedSnapshot.count(refA)) {
		if (g_pendingEvents.size() < kMaxQueuedEvents)
			g_pendingEvents.push_back({refA, refB, kChannel_RigidBody, true});
	}
	if (refB && g_watchedSnapshot.count(refB)) {
		if (g_pendingEvents.size() < kMaxQueuedEvents)
			g_pendingEvents.push_back({refB, refA, kChannel_RigidBody, true});
	}
}

//--- channel 1: character proxy contacts ---
//TODO: hook hkpCharacterProxy::fireContactAdded (0xCAD480) / fireContactRemoved (0xCAD4C0)

//--- channel 2: phantom overlaps ---
//TODO: hook add/removeOverlappingCollidable for simple and caching phantoms

//--- init / update / state ---

namespace OnContactHandler {

bool Init(void* nvseInterface)
{
	auto* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	InitCriticalSectionOnce(&g_lockInit, &g_contactLock);
	g_mainThreadId = GetCurrentThreadId();

	//channel 0: hook FOCollisionListener::contactPointAddedCallback
	//prologue at 0x623CB0: 55 8B EC 83 EC 7C (6 bytes)
	if (!s_rigidBodyDetour.WriteRelJump(0x623CB0, Hook_ContactPointAdded, 6)) {
		Log("OnContactHandler: failed to hook FOCollisionListener at 0x623CB0");
		return false;
	}

	Log("OnContactHandler: channel 0 (rigid body) installed");
	return true;
}

void Update()
{
	if (g_lockInit != 2) return;

	DWORD tid = GetCurrentThreadId();
	if (!g_mainThreadId) g_mainThreadId = tid;
	if (tid != g_mainThreadId) return;

	//refresh snapshot if watch set changed
	if (g_snapshotDirty) {
		ScopedLock lock(&g_contactLock);
		g_watchedSnapshot = g_watchedRefIDs;
		g_snapshotDirty = false;
	}

	//drain queue
	std::vector<QueuedContactEvent> events;
	{
		ScopedLock lock(&g_contactLock);
		events.swap(g_pendingEvents);
	}

	for (const auto& evt : events) {
		ContactPair pair = {evt.watchedRefID, evt.otherRefID, evt.channel};

		if (evt.isBegin) {
			int& count = g_activeContacts[pair];
			count++;
			if (count != 1) continue; //already active
		} else {
			auto it = g_activeContacts.find(pair);
			if (it == g_activeContacts.end()) continue;
			it->second--;
			if (it->second > 0) continue; //still active points
			g_activeContacts.erase(it);
		}

		auto* watched = (TESObjectREFR*)Engine::LookupFormByID(evt.watchedRefID);
		if (!watched) continue;

		auto* other = evt.otherRefID ? (TESObjectREFR*)Engine::LookupFormByID(evt.otherRefID) : nullptr;

		if (g_eventManagerInterface) {
			const char* eventName = evt.isBegin ? "ITR:OnContactBegin" : "ITR:OnContactEnd";
			g_eventManagerInterface->DispatchEvent(eventName, watched,
				(TESForm*)other, (int)evt.channel);
		}
	}

	//TODO: channel 0 end detection - poll rigid body contacts for watched refs,
	//diff against g_activeContacts, queue end events for pairs that disappeared
}

void ClearState()
{
	if (g_lockInit != 2) return;
	ScopedLock lock(&g_contactLock);
	g_pendingEvents.clear();
	g_activeContacts.clear();
	g_watchedRefIDs.clear();
	g_watchedSnapshot.clear();
	g_snapshotDirty = false;
}

void AddWatch(UInt32 refID)
{
	g_watchedRefIDs.insert(refID);
	g_snapshotDirty = true;
}

void RemoveWatch(UInt32 refID)
{
	g_watchedRefIDs.erase(refID);
	g_snapshotDirty = true;
	//only remove pairs where this ref is the WATCHED side (refA)
	for (auto it = g_activeContacts.begin(); it != g_activeContacts.end();) {
		if (it->first.refA == refID)
			it = g_activeContacts.erase(it);
		else
			++it;
	}
}

bool IsWatched(UInt32 refID)
{
	return g_watchedRefIDs.count(refID) != 0;
}

} //namespace OnContactHandler
