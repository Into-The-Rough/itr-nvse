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
#include <algorithm>
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

//channel 2: map havok phantom address -> actor refID (character phantoms aren't in NiNode tree)
static std::unordered_map<void*, UInt32> g_phantomToRefID;  //read by hooks under lock

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

//collidable -> TESObjectREFR refID
//tries rigid body path first, falls back to phantom/general path
typedef void* (__cdecl *_bhkNiCollisionObject_Getbhk)(void* worldObj);
static const _bhkNiCollisionObject_Getbhk bhkNiCollisionObject_Getbhk = (_bhkNiCollisionObject_Getbhk)0x4B5A20;

static UInt32 ResolveCollidableToRefID(void* collidable) {
	if (!collidable) return 0;
	void* root = getRoot(collidable);
	if (!root) return 0;

	//try rigid body path (works for non-phantom objects)
	void* rigidBody = hkpGetRigidBody(root);
	if (rigidBody) {
		void* collObj = GetbhkCollisionObject(rigidBody);
		if (collObj) {
			void* niNode = *(void**)((UInt8*)collObj + 0x08);
			if (niNode) {
				void* refr = FindReferenceFor3D(niNode);
				if (refr) return *(UInt32*)((UInt8*)refr + 0x0C);
			}
		}
	}

	//phantom/general path: hkpWorldObject → bhk wrapper via -0x10 → vtable GetObject → NiNode
	//hkpCollidable is embedded at hkpWorldObject+0x10, so worldObj = root - 0x10
	void* worldObj = (void*)((UInt8*)root - 0x10);
	//bhk wrapper = worldObj - 0x08 (bhkRefObject stores phkObject at +0x08)
	//JIP uses: sub ecx, 0x10; call vtable[6] (GetObject)
	//we replicate: bhkWrapper = havokObj - 0x08... not reliable for all types
	//safer: check if the root collidable's broadphase owner is a phantom
	//and use bhkNiCollisionObject_Getbhk which handles both cases
	void* collObj = bhkNiCollisionObject_Getbhk(worldObj);
	if (collObj) {
		void* niNode = *(void**)((UInt8*)collObj + 0x08);
		if (niNode) {
			void* refr = FindReferenceFor3D(niNode);
			if (refr) return *(UInt32*)((UInt8*)refr + 0x0C);
		}
	}

	return 0;
}

//hkpWorldObject -> TESObjectREFR refID
static UInt32 ResolveWorldObjToRefID(void* worldObj) {
	if (!worldObj) return 0;
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

	if (refA == refB) return; //skip self-contact
	if (refA && g_watchedSnapshot.count(refA))
		QueueEvent(refA, refB, kChannel_RigidBody, true);
	if (refB && g_watchedSnapshot.count(refB))
		QueueEvent(refB, refA, kChannel_RigidBody, true);
}

static void* GetActorController(void* form) {
	void* process = *(void**)((UInt8*)form + 0x68);
	if (!process) return nullptr;
	if (*(UInt32*)((UInt8*)process + 0x28) > 1) return nullptr;
	return *(void**)((UInt8*)process + 0x138);
}

//end detection for ch0 (rigid body) and ch1 (character proxy):
//poll actor's pointCollector.contactBodies + bodyUnderFeet and compare
//against active pairs. stale pairs get end events.
//non-actor ch0 pairs are expired immediately (can't poll their contacts).
static void PollContactEnd() {
	std::vector<ContactPair> stale;

	for (auto it = g_activeContacts.begin(); it != g_activeContacts.end(); ++it) {
		if (it->first.channel != kChannel_RigidBody && it->first.channel != kChannel_CharProxy)
			continue;

		auto* form = (TESObjectREFR*)Engine::LookupFormByID(it->first.refA);
		if (!form) { stale.push_back(it->first); continue; }

		UInt8 typeID = *((UInt8*)form + 0x04);
		if (typeID != 0x3B && typeID != 0x3C) {
			stale.push_back(it->first); //non-actor, can't poll
			continue;
		}

		void* ctrl = GetActorController(form);
		if (!ctrl) { stale.push_back(it->first); continue; }

		//check pointCollector.contactBodies + bodyUnderFeet
		void** bodies = *(void***)((UInt8*)ctrl + 0x3B4);
		UInt32 count = *(UInt32*)((UInt8*)ctrl + 0x3B8);

		bool found = false;
		for (UInt32 i = 0; i < count && bodies; i++) {
			if (!bodies[i]) continue;
			UInt32 contactRefID = ResolveWorldObjToRefID(bodies[i]);
			if (contactRefID == it->first.refB) { found = true; break; }
		}

		if (!found) {
			void* bodyUnderFeet = *(void**)((UInt8*)ctrl + 0x60C);
			UInt8 noContact = *((UInt8*)ctrl + 0x608);
			if (!noContact && bodyUnderFeet) {
				UInt32 feetRefID = ResolveWorldObjToRefID(bodyUnderFeet);
				if (feetRefID == it->first.refB) found = true;
			}
		}

		if (!found) stale.push_back(it->first);
	}

	for (const auto& pair : stale) {
		g_activeContacts.erase(pair);
		if (!g_watchedRefIDs.count(pair.refA)) continue;
		auto* watched = (TESObjectREFR*)Engine::LookupFormByID(pair.refA);
		auto* other = pair.refB ? (TESObjectREFR*)Engine::LookupFormByID(pair.refB) : nullptr;
		if (watched && g_eventManagerInterface)
			g_eventManagerInterface->DispatchEvent("ITR:OnContactEnd", watched,
				(TESForm*)other, (int)pair.channel);
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
	//hkpRootCdPoint: m_rootCollidableB at +0x48 (verified from bhkCharacterListener disasm)
	void* otherCollidable = *(void**)((UInt8*)point + 0x48);
	UInt32 otherRefID = ResolveCollidableToRefID(otherCollidable);

	if (otherRefID == actorRefID) return; //skip self-contact
	QueueEvent(actorRefID, otherRefID, kChannel_CharProxy, true);
}

static void __fastcall Hook_CharProxyContactRemoved(void* proxy, void* edx, void* point) {
	s_charRemovedDetour.GetTrampoline<_fireContact>()(proxy, point);
	//don't queue from the removed callback - collidable may be freed/invalid,
	//and we can't identify WHICH specific contact was removed.
	//ch1 end detection is handled by polling in PollRigidBodyContactEnd instead.
}

//--- channel 2: phantom overlaps ---

static Detours::JumpDetour s_simpleAddDetour;
static Detours::JumpDetour s_simpleRemoveDetour;
static Detours::JumpDetour s_cachingAddDetour;
static Detours::JumpDetour s_cachingRemoveDetour;

typedef void (__thiscall *_phantomOverlap)(void*, void*);

static UInt32 ResolvePhantomOrCollidable(void* havokObj) {
	if (!havokObj) return 0;
	//check phantom map first (character phantoms aren't in NiNode tree)
	auto it = g_phantomToRefID.find(havokObj);
	if (it != g_phantomToRefID.end()) return it->second;
	//fall back to standard resolution
	return ResolveWorldObjToRefID(havokObj);
}

static void PhantomOverlapCommon(void* phantom, void* cdBody, bool isAdd) {
	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;

	UInt32 phantomRefID = ResolvePhantomOrCollidable(phantom);

	//cdBody is hkCdBody*, walk to root collidable
	//the root might be another phantom (actor-to-actor) or a rigid body
	void* rootCollidable = cdBody ? getRoot(cdBody) : nullptr;
	UInt32 bodyRefID = 0;
	if (rootCollidable) {
		//root collidable is at worldObj+0x10, so worldObj = root-0x10
		void* bodyWorldObj = (void*)((UInt8*)rootCollidable - 0x10);
		bodyRefID = ResolvePhantomOrCollidable(bodyWorldObj);
		if (!bodyRefID)
			bodyRefID = ResolveCollidableToRefID(rootCollidable);
	}

	if (phantomRefID == bodyRefID) return; //skip self-contact
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

static void MapActorPhantom(void* form, UInt32 refID,
	std::unordered_map<void*, UInt32>& phantomMap)
{
	void* ctrl = GetActorController(form);
	if (!ctrl) return;
	void* chrPhantom = *(void**)((UInt8*)ctrl + 0x594);
	if (!chrPhantom) return;
	void* havokPhantom = *(void**)((UInt8*)chrPhantom + 0x08);
	if (havokPhantom) phantomMap[havokPhantom] = refID;
}

static void* g_processManager = (void*)0x11E0E80;
struct ProcessManagerLite {
	UInt32 unk000;
	struct { void** vtbl; void** data; UInt16 capacity; UInt16 firstFreeEntry; UInt16 numObjs; UInt16 growSize; } objects;
	UInt32 beginOffsets[4];
	UInt32 endOffsets[4];
};

static void RebuildProxyMap() {
	std::unordered_map<void*, UInt32> newProxyMap;
	std::unordered_map<void*, UInt32> newPhantomMap;

	//proxy map: WATCHED actors only (ch1 hook matching)
	for (auto refID : g_watchedRefIDs) {
		auto* form = (TESObjectREFR*)Engine::LookupFormByID(refID);
		if (!form) continue;
		UInt8 typeID = *((UInt8*)form + 0x04);
		if (typeID != 0x3B && typeID != 0x3C) continue;
		void* ctrl = GetActorController(form);
		if (!ctrl) continue;
		void* proxy = *(void**)((UInt8*)ctrl + 0x08);
		if (proxy) newProxyMap[proxy] = refID;
		MapActorPhantom(form, refID, newPhantomMap);
	}

	//phantom map: ALL loaded actors (for ch2 "other" side resolution)
	//proxy map is NOT populated for unwatched actors
	auto* pm = reinterpret_cast<ProcessManagerLite*>(g_processManager);
	if (pm && pm->objects.data) {
		UInt32 upper = pm->objects.firstFreeEntry;
		for (int bucket = 0; bucket < 2; bucket++) {
			UInt32 begin = pm->beginOffsets[bucket];
			UInt32 end = pm->endOffsets[bucket];
			if (begin > upper) begin = upper;
			if (end > upper) end = upper;
			for (UInt32 i = begin; i < end; i++) {
				void* actor = pm->objects.data[i];
				if (!actor) continue;
				UInt8 typeID = *((UInt8*)actor + 0x04);
				if (typeID != 0x3B && typeID != 0x3C) continue;
				UInt32 refID = *(UInt32*)((UInt8*)actor + 0x0C);
				if (!refID) continue;
				MapActorPhantom(actor, refID, newPhantomMap);
			}
		}
		void** playerPtr = (void**)0x11DEA3C;
		if (*playerPtr) {
			UInt32 playerRefID = *(UInt32*)((UInt8*)*playerPtr + 0x0C);
			MapActorPhantom(*playerPtr, playerRefID, newPhantomMap);
		}
	}

	ScopedLock lock(&g_contactLock);
	g_proxyToRefID.swap(newProxyMap);
	g_phantomToRefID.swap(newPhantomMap);
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

	//refresh snapshot and proxy map when watch set changes
	static int s_proxyMapTimer = 0;
	if (g_snapshotDirty) {
		{
			ScopedLock lock(&g_contactLock);
			g_watchedSnapshot = g_watchedRefIDs;
		}
		RebuildProxyMap();
		g_snapshotDirty = false;
		s_proxyMapTimer = 0;

		//seed initial contacts for newly watched actors
		std::vector<QueuedContactEvent> g_seedEvents;
		//fireContactAdded only fires for NEW contacts, so existing ones
		//at the time of watch enable need to be detected here
		for (auto refID : g_watchedRefIDs) {
			auto* form = (TESObjectREFR*)Engine::LookupFormByID(refID);
			if (!form) continue;
			UInt8 typeID = *((UInt8*)form + 0x04);
			if (typeID != 0x3B && typeID != 0x3C) continue;

			void* process = *(void**)((UInt8*)form + 0x68);
			if (!process) continue;
			if (*(UInt32*)((UInt8*)process + 0x28) > 1) continue;
			void* ctrl = *(void**)((UInt8*)process + 0x138);
			if (!ctrl) continue;

			//seed existing contacts into a local vector (not dispatched inline -
			//handlers could mutate watch state during dispatch)
			void** bodies = *(void***)((UInt8*)ctrl + 0x3B4);
			UInt32 count = *(UInt32*)((UInt8*)ctrl + 0x3B8);
			for (UInt32 i = 0; i < count && bodies; i++) {
				if (!bodies[i]) continue;
				UInt32 otherRefID = ResolveWorldObjToRefID(bodies[i]);
				if (otherRefID && otherRefID != refID)
					g_seedEvents.push_back({refID, otherRefID, kChannel_CharProxy, true});
			}

			UInt8 noContact = *((UInt8*)ctrl + 0x608);
			void* bodyUnderFeet = *(void**)((UInt8*)ctrl + 0x60C);
			if (!noContact && bodyUnderFeet) {
				UInt32 feetRefID = ResolveWorldObjToRefID(bodyUnderFeet);
				if (feetRefID && feetRefID != refID)
					g_seedEvents.push_back({refID, feetRefID, kChannel_CharProxy, true});
			}
		}

		//inject seed events into the pending queue for normal dispatch
		if (!g_seedEvents.empty()) {
			ScopedLock lock(&g_contactLock);
			size_t room = kMaxQueuedEvents > g_pendingEvents.size() ? kMaxQueuedEvents - g_pendingEvents.size() : 0;
			size_t toAdd = g_seedEvents.size() < room ? g_seedEvents.size() : room;
			g_pendingEvents.insert(g_pendingEvents.end(), g_seedEvents.begin(), g_seedEvents.begin() + toAdd);
			if (toAdd < g_seedEvents.size())
				Log("OnContactHandler: seed capped, dropped %u events", (UInt32)(g_seedEvents.size() - toAdd));
		}
	}

	//also rebuild proxy map periodically for controller/proxy churn
	if (!g_watchedRefIDs.empty() && ++s_proxyMapTimer >= 60) {
		RebuildProxyMap();
		s_proxyMapTimer = 0;
	}

	//drain queue
	std::vector<QueuedContactEvent> events;
	{
		ScopedLock lock(&g_contactLock);
		events.swap(g_pendingEvents);
	}

	for (const auto& evt : events) {
		//re-check watch state before each dispatch - handles unwatching during
		//handler callbacks and stale events from proxy map lag
		if (!g_watchedRefIDs.count(evt.watchedRefID)) continue;

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
	PollContactEnd();
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
	g_phantomToRefID.clear();
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

	//flush queued events for this ref so stale begin/end don't fire
	{
		ScopedLock lock(&g_contactLock);
		g_pendingEvents.erase(
			std::remove_if(g_pendingEvents.begin(), g_pendingEvents.end(),
				[refID](const QueuedContactEvent& e) { return e.watchedRefID == refID; }),
			g_pendingEvents.end());
	}

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
