//polls bhkCharacterController state each frame to detect jumps and landings
//no vtable hooks - avoids shared-slot recursion with other plugins

#include <vector>
#include <cstdint>
#include <Windows.h>

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

//ProcessManager layout:
//+0x04: NiTArray<MobileObject*> objects (vtbl+0x00, data+0x04, capacity+0x08, firstFree+0x0A, numObjs+0x0C)
//+0x14: UInt32 beginOffsets[4] — 0:High, 1:MidHigh, 2:MidLow, 3:Low
//+0x24: UInt32 endOffsets[4]

struct TrackedController {
	void* charCtrl;
	UInt32 refID;
	UInt32 prevState;
	float prevFallTime;
};

static std::vector<TrackedController> g_tracked;
static bool g_initialized = false;

static UInt32 ReadState(const void* charCtrl) {
	return charCtrl ? *(const UInt32*)((const UInt8*)charCtrl + 0x3F0) : 0xFFFFFFFF;
}

static float ReadFallTime(const void* charCtrl) {
	return charCtrl ? *(const float*)((const UInt8*)charCtrl + 0x548) : 0.0f;
}

static bool IsReadableAddress(const void* ptr, size_t size = sizeof(void*)) {
	if (!ptr) return false;
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT) return false;
	DWORD p = mbi.Protect & 0xFF;
	if ((mbi.Protect & PAGE_GUARD) || p == PAGE_NOACCESS) return false;
	if (p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_WRITECOPY &&
		p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY)
		return false;
	auto start = reinterpret_cast<uintptr_t>(ptr);
	auto end = start + (size ? (size - 1) : 0);
	auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - 1;
	return end <= regionEnd;
}

static void* GetCharController(void* actor) {
	if (!actor || !IsReadableAddress(actor, 0x6C)) return nullptr;

	UInt8 typeID = *((UInt8*)actor + 4);
	if (typeID != 0x3B && typeID != 0x3C) return nullptr;

	void* process = *(void**)((UInt8*)actor + 0x68);
	if (!process || !IsReadableAddress(process, 0x13C)) return nullptr;

	UInt32 processLevel = *(UInt32*)((UInt8*)process + 0x28);
	if (processLevel > 1) return nullptr;

	return *(void**)((UInt8*)process + 0x138);
}

static void TryAddActor(std::vector<TrackedController>& out, void* actor) {
	if (!actor) return;
	void* ctrl = GetCharController(actor);
	if (!ctrl || !IsReadableAddress(ctrl, 0x550)) return;
	UInt32 refID = *(UInt32*)((UInt8*)actor + 0x0C);
	if (!refID) return;
	for (const auto& t : out)
		if (t.charCtrl == ctrl) return;
	out.push_back({ctrl, refID, ReadState(ctrl), 0.0f});
}

//iterate ProcessManager::objects NiTArray for buckets 0 (High) and 1 (MidHigh)
static void CollectFromProcessManager(std::vector<TrackedController>& out) {
	if (!g_processManager) return;

	UInt8* pm = (UInt8*)g_processManager;
	void** arr = *(void***)(pm + 0x08); //NiTArray::data (ProcessManager+0x04 vtbl, +0x08 data)
	if (!arr) return;

	UInt32* beginOffsets = (UInt32*)(pm + 0x14);
	UInt32* endOffsets = (UInt32*)(pm + 0x24);

	//bucket 0 = High process, bucket 1 = MidHigh process
	for (int bucket = 0; bucket < 2; bucket++) {
		UInt32 begin = beginOffsets[bucket];
		UInt32 end = endOffsets[bucket];
		for (UInt32 i = begin; i < end; i++)
			TryAddActor(out, arr[i]);
	}
}

static void RebuildTracked() {
	std::vector<TrackedController> fresh;
	fresh.reserve(64);

	CollectFromProcessManager(fresh);

	void* player = g_thePlayer ? *g_thePlayer : nullptr;
	if (player)
		TryAddActor(fresh, player);

	for (auto& f : fresh) {
		for (const auto& old : g_tracked) {
			if (old.charCtrl == f.charCtrl) {
				f.prevState = old.prevState;
				f.prevFallTime = old.prevFallTime;
				break;
			}
		}
	}

	g_tracked = std::move(fresh);
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

	RebuildTracked();

	//scan and collect events without dispatching - a handler could
	//unload cells/actors and invalidate other entries mid-iteration
	std::vector<PendingEvent> pending;

	for (auto& t : g_tracked) {
		UInt32 curState = ReadState(t.charCtrl);
		float curFallTime = ReadFallTime(t.charCtrl);
		if (curState == 0xFFFFFFFF) {
			t.prevState = curState;
			continue;
		}

		if (curState == kHkState_Jumping && t.prevState != kHkState_Jumping)
			pending.push_back({1, t.refID, 0.0f});

		//mirrors old hook: newState == 0 || (newState & 0x2) == 0
		bool wasAirborne = (t.prevState == kHkState_Jumping || t.prevState == kHkState_InAir);
		bool nowGrounded = (curState == 0 || (curState & 0x2) == 0);
		if (wasAirborne && nowGrounded && curState != kHkState_Jumping)
			pending.push_back({2, t.refID, t.prevFallTime});

		t.prevFallTime = curFallTime;
		t.prevState = curState;
	}

	//dispatch after iteration - no controller pointers touched from here
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
