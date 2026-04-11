//Havok callbacks can arrive off the main thread.
//Queue contact events here and dispatch them from Update().
//g_activeContacts coalesces multiple contact points for the same watched pair.

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
	kChannel_DeadActorProximity = 3,
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

struct DeadActorCandidate {
	UInt32 refID;
	TESObjectREFR* ref;
	float posX;
	float posY;
	float posZ;
};

static std::unordered_set<UInt32> g_watchedRefIDs;         //main thread only (mutation)
static std::unordered_set<UInt32> g_watchedSnapshot;       //read by hooks under lock
static std::unordered_map<ContactPair, int, ContactPairHash> g_activeContacts;
static std::vector<QueuedContactEvent> g_pendingEvents;
static CRITICAL_SECTION g_contactLock;
static volatile LONG g_lockInit = 0;
static DWORD g_mainThreadId = 0;
static bool g_snapshotDirty = true;

static std::unordered_map<void*, UInt32> g_proxyToRefID;   //read by hooks under lock
static std::unordered_map<void*, UInt32> g_phantomToRefID;  //read by hooks under lock

static constexpr size_t kMaxQueuedEvents = 2048;
static constexpr float kDeadActorContactRadiusSq = 56.0f * 56.0f;
static constexpr float kDeadActorContactMaxZBelow = 20.0f;
static constexpr float kDeadActorContactMaxZAbove = 56.0f;

typedef void* (__thiscall *_getRoot)(void*);
typedef void* (__cdecl *_hkpGetRigidBody)(void* collidable);
typedef void* (__cdecl *_GetbhkCollisionObject)(void* worldObj);
typedef void* (__cdecl *_FindReferenceFor3D)(void* niNode);

static const _getRoot getRoot = (_getRoot)0x624020;
static const _hkpGetRigidBody hkpGetRigidBody = (_hkpGetRigidBody)0x4B59F0;
static const _GetbhkCollisionObject GetbhkCollisionObject = (_GetbhkCollisionObject)0x4B5A20;
static const _FindReferenceFor3D FindReferenceFor3D = (_FindReferenceFor3D)0x56F930;

typedef void* (__cdecl *_bhkNiCollisionObject_Getbhk)(void* worldObj);
static const _bhkNiCollisionObject_Getbhk bhkNiCollisionObject_Getbhk = (_bhkNiCollisionObject_Getbhk)0x4B5A20;

static UInt32 ResolveCollidableToRefID(void* collidable) {
	if (!collidable) return 0;
	void* root = getRoot(collidable);
	if (!root) return 0;

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

	//hkpCollidable is embedded at hkpWorldObject+0x10, so worldObj = root - 0x10
	void* worldObj = (void*)((UInt8*)root - 0x10);
	//Use bhkNiCollisionObject_Getbhk here; the manual wrapper walk is not reliable for phantoms.
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

static UInt32 ResolveWorldObjToRefID(void* worldObj) {
	if (!worldObj) return 0;
	void* collidable = (void*)((UInt8*)worldObj + 0x10);
	return ResolveCollidableToRefID(collidable);
}

static void QueueEvent(UInt32 watchedRefID, UInt32 otherRefID, UInt8 channel, bool isBegin) {
	//caller must already hold g_contactLock
	if (g_pendingEvents.size() >= kMaxQueuedEvents)
		return;
	g_pendingEvents.push_back({watchedRefID, otherRefID, channel, isBegin});
}

static UInt32 LookupMappedRefID(const std::unordered_map<void*, UInt32>& refMap, void* key) {
	auto it = refMap.find(key);
	return it != refMap.end() ? it->second : 0;
}

static Detours::JumpDetour s_rigidBodyDetour;
typedef void (__thiscall *_FOCollisionListener_contactPointAdded)(void*, void*);

static void __fastcall Hook_ContactPointAdded(void* listener, void* edx, void* event) {
	s_rigidBodyDetour.GetTrampoline<_FOCollisionListener_contactPointAdded>()(listener, event);

	//hkpContactPointAddedEvent: m_bodyA at +0x00, m_bodyB at +0x04
	void* bodyA = *(void**)((UInt8*)event + 0x00);
	void* bodyB = *(void**)((UInt8*)event + 0x04);

	UInt32 refA = ResolveCollidableToRefID(bodyA);
	UInt32 refB = ResolveCollidableToRefID(bodyB);

	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;
	if (refA == refB) return;
	if (refA && g_watchedSnapshot.count(refA))
		QueueEvent(refA, refB, kChannel_RigidBody, true);
	if (refB && g_watchedSnapshot.count(refB))
		QueueEvent(refB, refA, kChannel_RigidBody, true);
}

static bool IsActorTypeID(UInt8 typeID) {
	return typeID == 0x3B || typeID == 0x3C;
}

static bool IsDeadActor(void* actor) {
	return actor && (*(UInt32*)((UInt8*)actor + 0x108) == 2);
}

static bool HasLoadedRootNode(void* ref) {
	if (!ref) return false;
	void* renderState = *(void**)((UInt8*)ref + 0x64);
	return renderState && (*(void**)((UInt8*)renderState + 0x14) != nullptr);
}

static float GetRefPosX(void* ref) { return *(float*)((UInt8*)ref + 0x30); }
static float GetRefPosY(void* ref) { return *(float*)((UInt8*)ref + 0x34); }
static float GetRefPosZ(void* ref) { return *(float*)((UInt8*)ref + 0x38); }
static UInt32 GetRefID(void* ref) { return *(UInt32*)((UInt8*)ref + 0x0C); }

static void* GetActorController(void* form) {
	void* process = *(void**)((UInt8*)form + 0x68);
	if (!process) return nullptr;
	if (*(UInt32*)((UInt8*)process + 0x28) > 1) return nullptr;
	return *(void**)((UInt8*)process + 0x138);
}

static bool HasRealContactPair(UInt32 watchedRefID, UInt32 otherRefID) {
	return
		g_activeContacts.find({watchedRefID, otherRefID, kChannel_RigidBody}) != g_activeContacts.end() ||
		g_activeContacts.find({watchedRefID, otherRefID, kChannel_CharProxy}) != g_activeContacts.end() ||
		g_activeContacts.find({watchedRefID, otherRefID, kChannel_Phantom}) != g_activeContacts.end();
}

static void DispatchContactEventDirect(UInt32 watchedRefID, UInt32 otherRefID, UInt8 channel, bool isBegin) {
	if (!g_watchedRefIDs.count(watchedRefID)) return;
	auto* watched = (TESObjectREFR*)Engine::LookupFormByID(watchedRefID);
	if (!watched) return;
	auto* other = otherRefID ? (TESObjectREFR*)Engine::LookupFormByID(otherRefID) : nullptr;
	if (!g_eventManagerInterface) return;
	const char* eventName = isBegin ? "ITR:OnContactBegin" : "ITR:OnContactEnd";
	g_eventManagerInterface->DispatchEvent(eventName, watched, (TESForm*)other, (int)channel);
}

static void* g_processManager = (void*)0x11E0E80;
struct ProcessManagerLite {
	UInt32 unk000;
	struct { void** vtbl; void** data; UInt16 capacity; UInt16 firstFreeEntry; UInt16 numObjs; UInt16 growSize; } objects;
	UInt32 beginOffsets[4];
	UInt32 endOffsets[4];
};

static void CollectLoadedDeadActors(std::vector<DeadActorCandidate>& out) {
	auto* pm = reinterpret_cast<ProcessManagerLite*>(g_processManager);
	if (!pm || !pm->objects.data) return;

	std::unordered_set<UInt32> seenRefIDs;
	seenRefIDs.reserve(32);

	UInt32 upper = pm->objects.firstFreeEntry;
	for (int bucket = 0; bucket < 2; bucket++) {
		UInt32 begin = pm->beginOffsets[bucket];
		UInt32 end = pm->endOffsets[bucket];
		if (begin > upper) begin = upper;
		if (end > upper) end = upper;
		for (UInt32 i = begin; i < end; i++) {
			auto* actor = (TESObjectREFR*)pm->objects.data[i];
			if (!actor) continue;
			if (!IsActorTypeID(*(UInt8*)((UInt8*)actor + 0x04))) continue;
			UInt32 refID = GetRefID(actor);
			if (!refID || !seenRefIDs.emplace(refID).second) continue;
			if (!IsDeadActor(actor) || !HasLoadedRootNode(actor)) continue;
			out.push_back({refID, actor, GetRefPosX(actor), GetRefPosY(actor), GetRefPosZ(actor)});
		}
	}
}

static bool ShouldSynthesizeDeadActorContact(TESObjectREFR* watched, const DeadActorCandidate& deadActor) {
	if (!watched || !deadActor.ref) return false;
	if (GetRefID(watched) == deadActor.refID) return false;
	if (!HasLoadedRootNode(watched)) return false;
	if (IsDeadActor(watched)) return false;

	float dx = GetRefPosX(watched) - deadActor.posX;
	float dy = GetRefPosY(watched) - deadActor.posY;
	float dz = GetRefPosZ(watched) - deadActor.posZ;
	float distSq = dx * dx + dy * dy;
	if (distSq > kDeadActorContactRadiusSq) return false;
	if (dz < -kDeadActorContactMaxZBelow) return false;
	if (dz > kDeadActorContactMaxZAbove) return false;
	if (HasRealContactPair(GetRefID(watched), deadActor.refID)) return false;
	return true;
}

static void PollDeadActorProximityContacts() {
	if (g_watchedRefIDs.empty()) return;

	std::vector<DeadActorCandidate> deadActors;
	CollectLoadedDeadActors(deadActors);
	if (deadActors.empty()) return;

	std::unordered_set<ContactPair, ContactPairHash> currentPairs;
	currentPairs.reserve(deadActors.size() * 2);

	for (auto refID : g_watchedRefIDs) {
		auto* watched = (TESObjectREFR*)Engine::LookupFormByID(refID);
		if (!watched) continue;
		if (!IsActorTypeID(*(UInt8*)((UInt8*)watched + 0x04))) continue;

		for (const auto& deadActor : deadActors) {
			if (ShouldSynthesizeDeadActorContact(watched, deadActor))
				currentPairs.emplace(ContactPair{refID, deadActor.refID, kChannel_DeadActorProximity});
		}
	}

	std::vector<ContactPair> begins;
	std::vector<ContactPair> stale;

	for (const auto& pair : currentPairs) {
		if (g_activeContacts.find(pair) == g_activeContacts.end())
			begins.push_back(pair);
	}

	for (const auto& [pair, count] : g_activeContacts) {
		if (pair.channel != kChannel_DeadActorProximity) continue;
		if (!currentPairs.count(pair))
			stale.push_back(pair);
	}

	for (const auto& pair : stale) {
		g_activeContacts.erase(pair);
		DispatchContactEventDirect(pair.refA, pair.refB, pair.channel, false);
	}

	for (const auto& pair : begins) {
		if (!g_watchedRefIDs.count(pair.refA)) continue;
		g_activeContacts[pair] = 1;
		DispatchContactEventDirect(pair.refA, pair.refB, pair.channel, true);
	}
}

//Removed callbacks are not enough to close rigid-body and proxy contacts reliably.
static void PollContactEnd() {
	std::vector<ContactPair> stale;

	for (auto it = g_activeContacts.begin(); it != g_activeContacts.end(); ++it) {
		if (it->first.channel != kChannel_RigidBody && it->first.channel != kChannel_CharProxy)
			continue;

		auto* form = (TESObjectREFR*)Engine::LookupFormByID(it->first.refA);
		if (!form) { stale.push_back(it->first); continue; }

		UInt8 typeID = *((UInt8*)form + 0x04);
		if (typeID != 0x3B && typeID != 0x3C) {
			stale.push_back(it->first);
			continue;
		}

		void* ctrl = GetActorController(form);
		if (!ctrl) { stale.push_back(it->first); continue; }

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

static Detours::JumpDetour s_charAddedDetour;
static Detours::JumpDetour s_charRemovedDetour;
typedef void (__thiscall *_fireContact)(void*, void*);

static void __fastcall Hook_CharProxyContactAdded(void* proxy, void* edx, void* point) {
	s_charAddedDetour.GetTrampoline<_fireContact>()(proxy, point);

	UInt32 actorRefID = 0;
	{
		ScopedLock lock(&g_contactLock);
		if (g_watchedSnapshot.empty()) return;
		actorRefID = LookupMappedRefID(g_proxyToRefID, proxy);
		if (!actorRefID) return;
	}

	//hkpRootCdPoint: m_rootCollidableB at +0x48 (verified from bhkCharacterListener disasm)
	void* otherCollidable = *(void**)((UInt8*)point + 0x48);
	UInt32 otherRefID = ResolveCollidableToRefID(otherCollidable);

	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;
	if (!g_watchedSnapshot.count(actorRefID)) return;
	if (otherRefID == actorRefID) return;
	QueueEvent(actorRefID, otherRefID, kChannel_CharProxy, true);
}

static void __fastcall Hook_CharProxyContactRemoved(void* proxy, void* edx, void* point) {
	s_charRemovedDetour.GetTrampoline<_fireContact>()(proxy, point);
	//The removed callback does not identify a stable contact pair; PollContactEnd handles ch1 ends.
}

static Detours::JumpDetour s_simpleAddDetour;
static Detours::JumpDetour s_simpleRemoveDetour;
static Detours::JumpDetour s_cachingAddDetour;
static Detours::JumpDetour s_cachingRemoveDetour;

typedef void (__thiscall *_phantomOverlap)(void*, void*);

static void PhantomOverlapCommon(void* phantom, void* cdBody, bool isAdd) {
	void* rootCollidable = cdBody ? getRoot(cdBody) : nullptr;
	void* bodyWorldObj = rootCollidable ? (void*)((UInt8*)rootCollidable - 0x10) : nullptr;
	UInt32 mappedPhantomRefID = 0;
	UInt32 mappedBodyRefID = 0;

	{
		ScopedLock lock(&g_contactLock);
		if (g_watchedSnapshot.empty()) return;
		mappedPhantomRefID = LookupMappedRefID(g_phantomToRefID, phantom);
		mappedBodyRefID = LookupMappedRefID(g_phantomToRefID, bodyWorldObj);
	}

	UInt32 phantomRefID = mappedPhantomRefID ? mappedPhantomRefID : ResolveWorldObjToRefID(phantom);
	UInt32 bodyRefID = 0;
	if (bodyWorldObj) {
		bodyRefID = mappedBodyRefID ? mappedBodyRefID : ResolveWorldObjToRefID(bodyWorldObj);
		if (!bodyRefID)
			bodyRefID = ResolveCollidableToRefID(rootCollidable);
	}

	ScopedLock lock(&g_contactLock);
	if (g_watchedSnapshot.empty()) return;
	if (phantomRefID == bodyRefID) return;
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

static void RebuildProxyMap() {
	std::unordered_map<void*, UInt32> newProxyMap;
	std::unordered_map<void*, UInt32> newPhantomMap;

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

namespace OnContactHandler {

bool Init(void* nvseInterface)
{
	auto* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	InitCriticalSectionOnce(&g_lockInit, &g_contactLock);
	g_mainThreadId = GetCurrentThreadId();

	//prologue: 55 8B EC 83 EC 7C (6 bytes)
	if (!s_rigidBodyDetour.WriteRelJump(0x623CB0, Hook_ContactPointAdded, 6)) {
		Log("OnContactHandler: ch0 hook failed at 0x623CB0");
		return false;
	}

	//both prologues: 56 57 8B F9 8B B7 84 00 00 00 (need 10 bytes for clean boundary)
	if (!s_charAddedDetour.WriteRelJump(0xCAD480, Hook_CharProxyContactAdded, 10)) {
		Log("OnContactHandler: ch1 added hook failed at 0xCAD480");
		return false;
	}
	if (!s_charRemovedDetour.WriteRelJump(0xCAD4C0, Hook_CharProxyContactRemoved, 10)) {
		Log("OnContactHandler: ch1 removed hook failed at 0xCAD4C0");
		return false;
	}

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

	return true;
}

void Update()
{
	if (g_lockInit != 2) return;

	DWORD tid = GetCurrentThreadId();
	if (!g_mainThreadId) g_mainThreadId = tid;
	if (tid != g_mainThreadId) return;

	static int s_proxyMapTimer = 0;
	if (g_snapshotDirty) {
		{
			ScopedLock lock(&g_contactLock);
			g_watchedSnapshot = g_watchedRefIDs;
		}
		RebuildProxyMap();
		g_snapshotDirty = false;
		s_proxyMapTimer = 0;

		std::vector<QueuedContactEvent> g_seedEvents;
		//Seed existing contacts when watch state changes; add callbacks only see new contacts.
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

			//Collect seed events first so handlers cannot mutate watch state mid-scan.
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

			if (!g_seedEvents.empty()) {
			ScopedLock lock(&g_contactLock);
			size_t room = kMaxQueuedEvents > g_pendingEvents.size() ? kMaxQueuedEvents - g_pendingEvents.size() : 0;
			size_t toAdd = g_seedEvents.size() < room ? g_seedEvents.size() : room;
			g_pendingEvents.insert(g_pendingEvents.end(), g_seedEvents.begin(), g_seedEvents.begin() + toAdd);
		}
	}

	if (!g_watchedRefIDs.empty() && ++s_proxyMapTimer >= 60) {
		RebuildProxyMap();
		s_proxyMapTimer = 0;
	}

	std::vector<QueuedContactEvent> events;
	{
		ScopedLock lock(&g_contactLock);
		events.swap(g_pendingEvents);
	}

	for (const auto& evt : events) {
		//Watch state can change during handler callbacks.
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

	PollContactEnd();
	PollDeadActorProximityContacts();
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

	{
		ScopedLock lock(&g_contactLock);
		g_pendingEvents.erase(
			std::remove_if(g_pendingEvents.begin(), g_pendingEvents.end(),
				[refID](const QueuedContactEvent& e) { return e.watchedRefID == refID; }),
			g_pendingEvents.end());
	}

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
