#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <Windows.h>

#include "OnJumpLandHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"

constexpr UInt32 kVtbl_bhkCharacterStateInAir = 0x10CB36C;
constexpr UInt32 kVtbl_bhkCharacterStateJumping = 0x10CB398;
constexpr UInt32 kVFuncIdx_UpdateVelocity = 8;
constexpr size_t kMaxPendingEvents = 256;

struct ListNode
{
    void* item;
    ListNode* next;
};

static void* g_actorProcessManager = (void*)0x11E0E80;
static void** g_thePlayer = (void**)0x011DEA3C;

enum JumpLandEventType : UInt8
{
    kEvent_JumpStart = 1,
    kEvent_Landed = 2,
};

struct QueuedEvent
{
    UInt8 eventType;
    UInt32 actorRefID;
    float preClearFallTime;
};

struct ControllerMapping
{
    void* charCtrl;
    UInt32 refID;
};

namespace OnJumpLandHandler
{
    std::vector<QueuedEvent> g_pendingEvents;
    std::vector<void*> g_activeJumpControllers;
    std::vector<ControllerMapping> g_controllerMap;

    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;
    DWORD g_mainThreadId = 0;
    bool g_hooksInstalled = false;
}

static void EnsureStateLockInitialized()
{
    InitCriticalSectionOnce(&OnJumpLandHandler::g_stateLockInit, &OnJumpLandHandler::g_stateLock);
}

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
}

static UInt32 ReadControllerState(const void* charCtrl)
{
    return charCtrl ? *(const UInt32*)((const UInt8*)charCtrl + 0x3F0) : 0xFFFFFFFF;
}

static float ReadControllerFallTime(const void* charCtrl)
{
    return charCtrl ? *(const float*)((const UInt8*)charCtrl + 0x548) : 0.0f;
}

static bool IsReadableAddress(const void* ptr, size_t size = sizeof(void*))
{
    if (!ptr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;

    const DWORD protect = mbi.Protect;
    if ((protect & PAGE_GUARD) != 0) return false;
    if ((protect & 0xFF) == PAGE_NOACCESS) return false;

    const DWORD baseProtect = protect & 0xFF;
    if (baseProtect != PAGE_READONLY && baseProtect != PAGE_READWRITE &&
        baseProtect != PAGE_WRITECOPY && baseProtect != PAGE_EXECUTE_READ &&
        baseProtect != PAGE_EXECUTE_READWRITE && baseProtect != PAGE_EXECUTE_WRITECOPY)
        return false;

    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = start + (size ? (size - 1) : 0);
    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionStart + mbi.RegionSize - 1;
    return start >= regionStart && end <= regionEnd;
}

static void AddControllerForActor(std::vector<ControllerMapping>& outMap, void* actor)
{
    if (!actor) return;
    if (!IsReadableAddress(actor, 0x6C)) return;

    const UInt8 typeID = *((UInt8*)actor + 4);
    if (typeID != 0x3B && typeID != 0x3C) return;

    void* process = *(void**)((UInt8*)actor + 0x68);
    if (!process) return;
    if (!IsReadableAddress(process, 0x13C)) return;

    UInt32 processLevel = *(UInt32*)((UInt8*)process + 0x28);
    if (processLevel > 1) return;

    void* charCtrl = *(void**)((UInt8*)process + 0x138);
    if (!charCtrl) return;

    UInt32 refID = ReadRefID(actor);
    if (!refID) return;

    for (const auto& mapping : outMap)
        if (mapping.charCtrl == charCtrl) return;

    outMap.push_back({charCtrl, refID});
}

static void CollectControllersFromList(std::vector<ControllerMapping>& outMap, UInt32 listOffset)
{
    if (!g_actorProcessManager) return;

    ListNode* node = (ListNode*)((UInt8*)g_actorProcessManager + listOffset);
    UInt32 guard = 0;
    while (node && guard++ < 4096) {
        if (!IsReadableAddress(node, sizeof(ListNode))) break;
        void* actor = node->item;
        ListNode* next = node->next;
        if (actor) AddControllerForActor(outMap, actor);
        node = next;
    }
}

static void RebuildControllerMap()
{
    void* player = g_thePlayer ? *g_thePlayer : nullptr;
    if (!player) {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);
        OnJumpLandHandler::g_controllerMap.clear();
        return;
    }

    std::vector<ControllerMapping> newMap;
    newMap.reserve(64);

    CollectControllersFromList(newMap, 0x00);
    CollectControllersFromList(newMap, 0x5C);
    AddControllerForActor(newMap, player);

    ScopedLock lock(&OnJumpLandHandler::g_stateLock);
    OnJumpLandHandler::g_controllerMap = std::move(newMap);
}

static UInt32 LookupRefIDFromController(void* charCtrl)
{
    for (const auto& m : OnJumpLandHandler::g_controllerMap)
        if (m.charCtrl == charCtrl) return m.refID;
    return 0;
}

static bool QueuePendingEventLocked(UInt8 eventType, UInt32 actorRefID, float preClearFallTime)
{
    if (!actorRefID) return false;
    if (OnJumpLandHandler::g_pendingEvents.size() >= kMaxPendingEvents) return false;
    OnJumpLandHandler::g_pendingEvents.push_back({eventType, actorRefID, preClearFallTime});
    return true;
}

typedef void (__thiscall *StateUpdateVelocity_t)(void* state, void* charCtrl);
static StateUpdateVelocity_t s_originalJumpingUpdateVelocity = nullptr;
static StateUpdateVelocity_t s_originalInAirUpdateVelocity = nullptr;

static void __fastcall Hook_bhkCharacterStateJumping_UpdateVelocity(void* state, void* edx, void* charCtrl)
{
    if (s_originalJumpingUpdateVelocity)
        s_originalJumpingUpdateVelocity(state, charCtrl);

    if (!charCtrl || OnJumpLandHandler::g_stateLockInit != 2) return;

    ScopedLock lock(&OnJumpLandHandler::g_stateLock);

    auto& active = OnJumpLandHandler::g_activeJumpControllers;
    if (std::find(active.begin(), active.end(), charCtrl) != active.end()) return;

    UInt32 refID = LookupRefIDFromController(charCtrl);
    if (!refID) return;

    active.push_back(charCtrl);
    QueuePendingEventLocked(kEvent_JumpStart, refID, 0.0f);
}

static void __fastcall Hook_bhkCharacterStateInAir_UpdateVelocity(void* state, void* edx, void* charCtrl)
{
    const float preClearFallTime = ReadControllerFallTime(charCtrl);

    if (s_originalInAirUpdateVelocity)
        s_originalInAirUpdateVelocity(state, charCtrl);

    if (!charCtrl || OnJumpLandHandler::g_stateLockInit != 2) return;

    const UInt32 newState = ReadControllerState(charCtrl);
    if (newState == 0xFFFFFFFF) return;

    if (newState == 0 || (newState & 0x2) == 0)
    {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);

        auto& active = OnJumpLandHandler::g_activeJumpControllers;
        auto it = std::find(active.begin(), active.end(), charCtrl);
        if (it != active.end()) active.erase(it);

        UInt32 refID = LookupRefIDFromController(charCtrl);
        if (!refID) return;

        QueuePendingEventLocked(kEvent_Landed, refID, preClearFallTime);
    }
}

static bool InstallHooks()
{
    if (OnJumpLandHandler::g_hooksInstalled) return true;

    const UInt32 jumpSlot = kVtbl_bhkCharacterStateJumping + (kVFuncIdx_UpdateVelocity * 4);
    const UInt32 inAirSlot = kVtbl_bhkCharacterStateInAir + (kVFuncIdx_UpdateVelocity * 4);

    s_originalJumpingUpdateVelocity = reinterpret_cast<StateUpdateVelocity_t>(*(UInt32*)jumpSlot);
    s_originalInAirUpdateVelocity = reinterpret_cast<StateUpdateVelocity_t>(*(UInt32*)inAirSlot);

    if (!s_originalJumpingUpdateVelocity || !s_originalInAirUpdateVelocity)
        return false;

    SafeWrite::Write32(jumpSlot, (UInt32)Hook_bhkCharacterStateJumping_UpdateVelocity);
    SafeWrite::Write32(inAirSlot, (UInt32)Hook_bhkCharacterStateInAir_UpdateVelocity);

    OnJumpLandHandler::g_hooksInstalled = true;
    return true;
}

//vtable slots are write-once at init. if another plugin overwrites us, we lose
//our hook - but that's better than reclaiming and causing infinite recursion
//when both plugins save each other as their "original"

void OJLH_Update()
{
    if (OnJumpLandHandler::g_stateLockInit != 2) return;
    if (!g_eventManagerInterface) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnJumpLandHandler::g_mainThreadId)
        OnJumpLandHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnJumpLandHandler::g_mainThreadId)
        return;

    std::vector<QueuedEvent> queuedEvents;
    {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);
        queuedEvents.swap(OnJumpLandHandler::g_pendingEvents);
    }

    RebuildControllerMap();

    for (const auto& evt : queuedEvents) {
        void* actor = Engine::LookupFormByID(evt.actorRefID);
        if (!actor) continue;

        if (evt.eventType == kEvent_Landed) {
            g_eventManagerInterface->DispatchEvent("ITR:OnActorLanded",
                reinterpret_cast<TESObjectREFR*>(actor),
                (TESForm*)actor, PackEventFloatArg(evt.preClearFallTime));
        }
        else if (evt.eventType == kEvent_JumpStart) {
            g_eventManagerInterface->DispatchEvent("ITR:OnJumpStart",
                reinterpret_cast<TESObjectREFR*>(actor),
                (TESForm*)actor);
        }
    }
}

bool OJLH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    EnsureStateLockInitialized();
    OnJumpLandHandler::g_mainThreadId = GetCurrentThreadId();

    if (!InstallHooks())
        return false;

    return true;
}
