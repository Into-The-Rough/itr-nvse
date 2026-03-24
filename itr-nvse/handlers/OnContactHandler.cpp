//3-channel contact event system
//
//channel 0: rigid body contacts - hook FOCollisionListener::contactPointAddedCallback (0x623CB0)
//           end detection via per-frame polling of watched refs' contact arrays
//channel 1: character proxy contacts - hook hkpCharacterProxy::fireContactAdded/Removed
//           (0xCAD480/0xCAD4C0) with proxy-to-refID lookup map
//channel 2: phantom overlaps - hook hkpSimpleShapePhantom/hkpCachingShapePhantom
//           add/removeOverlappingCollidable (0xD1F690/0xD1F5A0/0xD4CF10/0xD4CCA0)
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

//all mutable state protected by g_contactLock when accessed from callbacks
static std::unordered_set<UInt32> g_watchedRefIDs;         //main thread only (mutation)
static std::unordered_set<UInt32> g_watchedSnapshot;       //read by hooks under lock
static std::unordered_map<ContactPair, int, ContactPairHash> g_activeContacts;
static std::vector<QueuedContactEvent> g_pendingEvents;
static CRITICAL_SECTION g_contactLock;
static volatile LONG g_lockInit = 0;
static DWORD g_mainThreadId = 0;
static bool g_snapshotDirty = true;

//channel 1: map havok proxy address -> actor refID for fast lookup in callbacks
static std::unordered_map<void*, UInt32> g_proxyToRefID;   //read by hooks under lock

static constexpr size_t kMaxQueuedEvents = 2048;

//--- ref resolution ---

typedef void* (__thiscall *_getRoot)(void*);
typedef void* (__cdecl *_hkpGetRigidBody)(void* collidable);
typedef void* (__cdecl *_GetbhkCollisionObject)(void* worldObj);
typedef void* (__cdecl *_FindReferenceFor3D)(void* niNode);

static const _getRoot getRoot = (_getRoot)0x624020;
static const _hkpGetRigidBody hkpGetRigidBody = (_hkpGetRigidBody)0x4B59F0;
static const _GetbhkCollisionObject GetbhkCollisionObject = (_GetbhkCollisionObject)0x4B5A20;
static const _FindReferenceFor3D FindReferenceFor3D = (_FindReferenceFor3D)0x56F930;

//collidable -> TESObjectREFR refID (rigid body path)
static UInt32 ResolveCollidableToRefID(void* collidable) {
	if (!collidable) return 0;
	void* root = getRoot(collidable);
	if (!root) return 0;
	void* rigidBody = hkpGetRigidBody(root);
	if (!rigidBody) return 0;
	void* collObj = GetbhkCollisionObject(rigidBody);
	if (!collObj) return 0;
	void* niNode = *(void**)((UInt8*)collObj + 0x08);
	if (!niNode) return 0;
	void* refr = FindReferenceFor3D(niNode);
	if (!refr) return 0;
	return *(UInt32*)((UInt8*)refr + 0x0C);
}

//hkpWorldObject -> TESObjectREFR refID (used for contactBodies in polling)
//same as JIP's GetParentRef: worldObj-0x10 → bhk wrapper → vtable GetObject → walk NiNode
static UInt32 ResolveWorldObjToRefID(void* worldObj) {
	if (!worldObj) return 0;
	//worldObj is hkpWorldObject. bhk wrapper is at worldObj-0x10
	//(bhkRefObject stores phkObject at +0x08, so phkObject = bhkWrapper+0x08)
	//bhkWrapper = phkObject - 0x08... but that's not right for all types.
	//use the collidable on the worldobject instead
	//hkpWorldObject has m_collidable at +0x10 (inline, not pointer)
	void* collidable = (void*)((UInt8*)worldObj + 0x10);
	return ResolveCollidableToRefID(collidable);
}

static void QueueEvent(UInt32 watchedRefID, UInt32 otherRefID, UInt8 channel, bool isBegin) {
	//caller must already hold g_contactLock
	if (g_pendingEvents.size() >= kMaxQueuedEvents) {
		Log("OnContactHandler: queue full, dropping %s for 0x%08X", isBegin ? "begin" : "end", watchedRefID);
		return;
	}
	g_pendingEvents.push_back({watchedRefID, otherRefID, channel, isBegin});
}

//--- channel 0: rigid body contacts ---

static Detours::JumpDetour s_rigidBodyDetour;
typedef void (__thiscall *_FOCollisionListener_contactPointAdded)(void*, void*);

static void __fastcall Hook_ContactPointAdded(void* listener, void* edx, void* event) {
	s_rigidBodyDetour.GetTrampoline<_FOCollisionListener_contactPointAdded>()(listener, event);

	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;

	//hkpContactPointAddedEvent: m_bodyA at +0x00, m_bodyB at +0x04
	void* bodyA = *(void**)((UInt8*)event + 0x00);
	void* bodyB = *(void**)((UInt8*)event + 0x04);

	UInt32 refA = ResolveCollidableToRefID(bodyA);
	UInt32 refB = ResolveCollidableToRefID(bodyB);

	if (refA && g_watchedSnapshot.count(refA))
		QueueEvent(refA, refB, kChannel_RigidBody, true);
	if (refB && g_watchedSnapshot.count(refB))
		QueueEvent(refB, refA, kChannel_RigidBody, true);
}

//channel 0 end detection: compare current rigid body contacts against active pairs
//uses Actor process's contact arrays, same approach as JIP's GetContactRefs
static void PollRigidBodyContactEnd() {
	//collect current rigid body contacts for each watched ref
	for (auto refID : g_watchedRefIDs) {
		auto* form = (TESObjectREFR*)Engine::LookupFormByID(refID);
		if (!form) continue;

		//get current contacts for this ref
		std::unordered_set<UInt32> currentContacts;

		UInt8 typeID = *((UInt8*)form + 0x04);
		bool isActor = (typeID == 0x3B || typeID == 0x3C);

		if (!isActor) {
			//non-actor: check renderState for rigid body contacts
			void* renderState = *(void**)((UInt8*)form + 0x64);
			if (!renderState) continue;
			void* rootNode = *(void**)((UInt8*)renderState + 0x14);
			if (!rootNode) continue;
			//TODO: walk NiNode contactObjects - complex, defer to later
			continue;
		}
		//actors use character controller contacts which is channel 1, not 0

		//find active channel-0 pairs for this watched ref and check for removals
		for (auto it = g_activeContacts.begin(); it != g_activeContacts.end();) {
			if (it->first.refA == refID && it->first.channel == kChannel_RigidBody) {
				if (!currentContacts.count(it->first.refB)) {
					//contact ended
					auto* watched = (TESObjectREFR*)Engine::LookupFormByID(refID);
					auto* other = it->first.refB ? (TESObjectREFR*)Engine::LookupFormByID(it->first.refB) : nullptr;
					if (watched && g_eventManagerInterface)
						g_eventManagerInterface->DispatchEvent("ITR:OnContactEnd", watched,
							(TESForm*)other, (int)kChannel_RigidBody);
					it = g_activeContacts.erase(it);
					continue;
				}
			}
			++it;
		}
	}
}

//--- channel 1: character proxy contacts ---

static Detours::JumpDetour s_charAddedDetour;
static Detours::JumpDetour s_charRemovedDetour;
typedef void (__thiscall *_fireContact)(void*, void*);

static void __fastcall Hook_CharProxyContactAdded(void* proxy, void* edx, void* point) {
	s_charAddedDetour.GetTrampoline<_fireContact>()(proxy, point);

	ScopedLock lock(&g_contactLock);
	auto it = g_proxyToRefID.find(proxy);
	if (it == g_proxyToRefID.end()) return;

	UInt32 actorRefID = it->second;
	//hkpRootCdPoint: m_rootCollidableB at +0x10
	void* otherCollidable = *(void**)((UInt8*)point + 0x10);
	UInt32 otherRefID = ResolveCollidableToRefID(otherCollidable);

	QueueEvent(actorRefID, otherRefID, kChannel_CharProxy, true);
}

static void __fastcall Hook_CharProxyContactRemoved(void* proxy, void* edx, void* point) {
	s_charRemovedDetour.GetTrampoline<_fireContact>()(proxy, point);

	ScopedLock lock(&g_contactLock);
	auto it = g_proxyToRefID.find(proxy);
	if (it == g_proxyToRefID.end()) return;

	UInt32 actorRefID = it->second;
	void* otherCollidable = *(void**)((UInt8*)point + 0x10);
	UInt32 otherRefID = ResolveCollidableToRefID(otherCollidable);

	QueueEvent(actorRefID, otherRefID, kChannel_CharProxy, false);
}

//--- channel 2: phantom overlaps ---

static Detours::JumpDetour s_simpleAddDetour;
static Detours::JumpDetour s_simpleRemoveDetour;
static Detours::JumpDetour s_cachingAddDetour;
static Detours::JumpDetour s_cachingRemoveDetour;

typedef void (__thiscall *_phantomOverlap)(void*, void*);

//resolve phantom (this) to refID via its collidable
static UInt32 ResolvePhantomToRefID(void* phantom) {
	if (!phantom) return 0;
	//hkpPhantom inherits hkpWorldObject, collidable at +0x10
	void* collidable = (void*)((UInt8*)phantom + 0x10);
	return ResolveCollidableToRefID(collidable);
}

static void PhantomOverlapCommon(void* phantom, void* cdBody, bool isAdd) {
	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;

	//cdBody (hkCdBody) has collidable we can resolve
	//hkCdBody: m_shape at +0x00, m_quality at +0x04, ... m_parent at +0x0C (hkpCollidable* or hkCdBody*)
	//actually for the overlap callback, cdBody IS a collidable pointer
	UInt32 phantomRefID = ResolvePhantomToRefID(phantom);
	UInt32 bodyRefID = ResolveCollidableToRefID(cdBody);

	if (phantomRefID && g_watchedSnapshot.count(phantomRefID))
		QueueEvent(phantomRefID, bodyRefID, kChannel_Phantom, isAdd);
	if (bodyRefID && g_watchedSnapshot.count(bodyRefID))
		QueueEvent(bodyRefID, phantomRefID, kChannel_Phantom, isAdd);
}

static void __fastcall Hook_SimpleAdd(void* phantom, void* edx, void* cdBody) {
	s_simpleAddDetour.GetTrampoline<_phantomOverlap>()(phantom, cdBody);
	PhantomOverlapCommon(phantom, cdBody, true);
}

static void __fastcall Hook_SimpleRemove(void* phantom, void* edx, void* cdBody) {
	s_simpleRemoveDetour.GetTrampoline<_phantomOverlap>()(phantom, cdBody);
	PhantomOverlapCommon(phantom, cdBody, false);
}

static void __fastcall Hook_CachingAdd(void* phantom, void* edx, void* cdBody) {
	s_cachingAddDetour.GetTrampoline<_phantomOverlap>()(phantom, cdBody);
	PhantomOverlapCommon(phantom, cdBody, true);
}

static void __fastcall Hook_CachingRemove(void* phantom, void* edx, void* cdBody) {
	s_cachingRemoveDetour.GetTrampoline<_phantomOverlap>()(phantom, cdBody);
	PhantomOverlapCommon(phantom, cdBody, false);
}

//--- proxy map maintenance ---

static void RebuildProxyMap() {
	g_proxyToRefID.clear();
	for (auto refID : g_watchedRefIDs) {
		auto* form = (TESObjectREFR*)Engine::LookupFormByID(refID);
		if (!form) continue;
		UInt8 typeID = *((UInt8*)form + 0x04);
		if (typeID != 0x3B && typeID != 0x3C) continue; //actors only

		//Actor+0x68 = process, process+0x138 = charController (high process only)
		void* process = *(void**)((UInt8*)form + 0x68);
		if (!process) continue;
		UInt32 processLevel = *(UInt32*)((UInt8*)process + 0x28);
		if (processLevel > 1) continue; //need high or middle-high

		void* ctrl = *(void**)((UInt8*)process + 0x138);
		if (!ctrl) continue;

		//bhkCharacterProxy::phkObject at +0x08 = hkpCharacterProxy*
		void* proxy = *(void**)((UInt8*)ctrl + 0x08);
		if (proxy) {
			ScopedLock lock(&g_contactLock);
			g_proxyToRefID[proxy] = refID;
		}
	}
}

//--- init / update / state ---

namespace OnContactHandler {

bool Init(void* nvseInterface)
{
	auto* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	InitCriticalSectionOnce(&g_lockInit, &g_contactLock);
	g_mainThreadId = GetCurrentThreadId();

	//channel 0: FOCollisionListener::contactPointAddedCallback
	//prologue: 55 8B EC 83 EC 7C (6 bytes)
	if (!s_rigidBodyDetour.WriteRelJump(0x623CB0, Hook_ContactPointAdded, 6)) {
		Log("OnContactHandler: ch0 hook failed at 0x623CB0");
		return false;
	}
	Log("OnContactHandler: channel 0 (rigid body) installed");

	//channel 1: hkpCharacterProxy::fireContactAdded/Removed
	//both prologues: 56 57 8B F9 8B B7 84 00 00 00 (need 10 bytes for clean boundary)
	if (!s_charAddedDetour.WriteRelJump(0xCAD480, Hook_CharProxyContactAdded, 10)) {
		Log("OnContactHandler: ch1 added hook failed at 0xCAD480");
		return false;
	}
	if (!s_charRemovedDetour.WriteRelJump(0xCAD4C0, Hook_CharProxyContactRemoved, 10)) {
		Log("OnContactHandler: ch1 removed hook failed at 0xCAD4C0");
		return false;
	}
	Log("OnContactHandler: channel 1 (character proxy) installed");

	//channel 2: phantom add/removeOverlappingCollidable
	//simple add: 53 8B 5C 24 08 83 3B 00 (need >=5 clean bytes)
	if (!s_simpleAddDetour.WriteRelJump(0xD1F690, Hook_SimpleAdd, 5)) {
		Log("OnContactHandler: ch2 simple-add hook failed at 0xD1F690");
		return false;
	}
	//simple remove: 56 57 8B F9 8B 4C 24 0C (need 8 clean bytes for 1+1+2+4)
	if (!s_simpleRemoveDetour.WriteRelJump(0xD1F5A0, Hook_SimpleRemove, 8)) {
		Log("OnContactHandler: ch2 simple-remove hook failed at 0xD1F5A0");
		return false;
	}
	//caching add: 55 8B EC 83 E4 F0 (6 bytes)
	if (!s_cachingAddDetour.WriteRelJump(0xD4CF10, Hook_CachingAdd, 6)) {
		Log("OnContactHandler: ch2 caching-add hook failed at 0xD4CF10");
		return false;
	}
	//caching remove: 83 EC 18 56 57 8B F9 8B (need 7 clean bytes: 3+1+1+2)
	if (!s_cachingRemoveDetour.WriteRelJump(0xD4CCA0, Hook_CachingRemove, 7)) {
		Log("OnContactHandler: ch2 caching-remove hook failed at 0xD4CCA0");
		return false;
	}
	Log("OnContactHandler: channel 2 (phantom) installed");

	return true;
}

void Update()
{
	if (g_lockInit != 2) return;

	DWORD tid = GetCurrentThreadId();
	if (!g_mainThreadId) g_mainThreadId = tid;
	if (tid != g_mainThreadId) return;

	//refresh snapshot and proxy map if watch set changed
	if (g_snapshotDirty) {
		{
			ScopedLock lock(&g_contactLock);
			g_watchedSnapshot = g_watchedRefIDs;
		}
		RebuildProxyMap();
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
			if (count != 1) continue;
		} else {
			auto it = g_activeContacts.find(pair);
			if (it == g_activeContacts.end()) continue;
			it->second--;
			if (it->second > 0) continue;
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

	//channel 0 end detection via polling
	PollRigidBodyContactEnd();
}

void ClearState()
{
	if (g_lockInit != 2) return;
	ScopedLock lock(&g_contactLock);
	g_pendingEvents.clear();
	g_activeContacts.clear();
	g_watchedRefIDs.clear();
	g_watchedSnapshot.clear();
	g_proxyToRefID.clear();
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
	//only remove pairs where this ref is the watched side
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
