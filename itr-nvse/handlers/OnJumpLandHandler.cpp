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

struct ListNode {
	void* item;
	ListNode* next;
};

static void* g_actorProcessManager = (void*)0x11E0E80;
static void** g_thePlayer = (void**)0x011DEA3C;

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

static void CollectFromList(std::vector<TrackedController>& out, UInt32 listOffset) {
	if (!g_actorProcessManager) return;

	ListNode* node = (ListNode*)((UInt8*)g_actorProcessManager + listOffset);
	UInt32 guard = 0;
	while (node && guard++ < 4096) {
		if (!IsReadableAddress(node, sizeof(ListNode))) break;
		void* actor = node->item;
		if (actor) {
			void* ctrl = GetCharController(actor);
			if (ctrl) {
				UInt32 refID = *(UInt32*)((UInt8*)actor + 0x0C);
				if (refID) {
					bool found = false;
					for (const auto& t : out)
						if (t.charCtrl == ctrl) { found = true; break; }
					if (!found)
						out.push_back({ctrl, refID, ReadState(ctrl), 0.0f});
				}
			}
		}
		node = node->next;
	}
}

static void RebuildTracked() {
	std::vector<TrackedController> fresh;
	fresh.reserve(64);

	CollectFromList(fresh, 0x00);
	CollectFromList(fresh, 0x5C);

	void* player = g_thePlayer ? *g_thePlayer : nullptr;
	if (player) {
		void* ctrl = GetCharController(player);
		if (ctrl) {
			UInt32 refID = *(UInt32*)((UInt8*)player + 0x0C);
			bool found = false;
			for (const auto& t : fresh)
				if (t.charCtrl == ctrl) { found = true; break; }
			if (!found)
				fresh.push_back({ctrl, refID, ReadState(ctrl), 0.0f});
		}
	}

	//carry over prevState from previous frame's tracking
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

void OJLH_Update()
{
	if (!g_eventManagerInterface) return;

	RebuildTracked();

	for (auto& t : g_tracked) {
		UInt32 curState = ReadState(t.charCtrl);
		float curFallTime = ReadFallTime(t.charCtrl);
		if (curState == 0xFFFFFFFF) {
			t.prevState = curState;
			continue;
		}

		//jump start: entered Jumping from any non-jumping state
		if (curState == kHkState_Jumping && t.prevState != kHkState_Jumping) {
			void* actor = Engine::LookupFormByID(t.refID);
			if (actor)
				g_eventManagerInterface->DispatchEvent("ITR:OnJumpStart",
					reinterpret_cast<TESObjectREFR*>(actor),
					(TESForm*)actor);
		}

		//landed: was airborne (Jumping/InAir), now not airborne
		//mirrors old hook: newState == 0 || (newState & 0x2) == 0
		//matches OnGround, Jumping(1), Swimming(5), Flying(4) but not Climbing(3) or Projectile(6)
		bool wasAirborne = (t.prevState == kHkState_Jumping || t.prevState == kHkState_InAir);
		bool nowGrounded = (curState == 0 || (curState & 0x2) == 0);
		if (wasAirborne && nowGrounded && curState != kHkState_Jumping) {
			void* actor = Engine::LookupFormByID(t.refID);
			if (actor)
				g_eventManagerInterface->DispatchEvent("ITR:OnActorLanded",
					reinterpret_cast<TESObjectREFR*>(actor),
					(TESForm*)actor, PackEventFloatArg(t.prevFallTime));
		}

		t.prevFallTime = curFallTime;
		t.prevState = curState;
	}
}

bool OJLH_Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;
	g_initialized = true;
	return true;
}
