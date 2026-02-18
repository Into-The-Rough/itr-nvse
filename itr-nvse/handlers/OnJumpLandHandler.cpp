#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <Windows.h>

#include "OnJumpLandHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/ScopedLock.h"

static NVSEScriptInterface* g_ojlhScript = nullptr;
static _CaptureLambdaVars g_CaptureLambdaVars = nullptr;
static _UncaptureLambdaVars g_UncaptureLambdaVars = nullptr;

static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ojlhLandedOpcode = 0;
static UInt32 g_ojlhJumpOpcode = 0;

constexpr UInt32 kVtbl_bhkCharacterStateInAir = 0x10CB36C;
constexpr UInt32 kVtbl_bhkCharacterStateJumping = 0x10CB398;
constexpr UInt32 kVFuncIdx_UpdateVelocity = 8;
constexpr UInt32 kOffset_RefID = 0x0C;
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

struct CallbackEntry
{
    Script* callback;
    TESForm* actorFilter;
    UInt32 callbackFormID;
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
    std::vector<CallbackEntry> g_landedCallbacks;
    std::vector<CallbackEntry> g_jumpCallbacks;
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
    if (OnJumpLandHandler::g_stateLockInit == 2) return;

    if (InterlockedCompareExchange(&OnJumpLandHandler::g_stateLockInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&OnJumpLandHandler::g_stateLock);
        InterlockedExchange(&OnJumpLandHandler::g_stateLockInit, 2);
        return;
    }

    while (OnJumpLandHandler::g_stateLockInit != 2)
        Sleep(0);
}

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + kOffset_RefID) : 0;
}

static TESForm* ReadBaseForm(void* actor)
{
    return actor ? *(TESForm**)((UInt8*)actor + 0x20) : nullptr;
}

static UInt32 ReadControllerState(const void* charCtrl)
{
    return charCtrl ? *(const UInt32*)((const UInt8*)charCtrl + 0x3F0) : 0xFFFFFFFF;
}

static float ReadControllerFallTime(const void* charCtrl)
{
    return charCtrl ? *(const float*)((const UInt8*)charCtrl + 0x548) : 0.0f;
}

static TESForm* LookupFormByID_Local(UInt32 refID)
{
    if (!refID) return nullptr;

    struct Entry {
        Entry* next;
        UInt32 key;
        TESForm* form;
    };

    UInt8* map = *(UInt8**)0x11C54C0;
    if (!map) return nullptr;

    UInt32 numBuckets = *(UInt32*)(map + 4);
    Entry** buckets = *(Entry***)(map + 8);
    if (!buckets || !numBuckets) return nullptr;

    for (Entry* e = buckets[refID % numBuckets]; e; e = e->next)
    {
        if (e->key == refID)
            return e->form;
    }

    return nullptr;
}

static bool IsReadableAddress(const void* ptr, size_t size = sizeof(void*))
{
    if (!ptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD protect = mbi.Protect;
    if ((protect & PAGE_GUARD) != 0)
        return false;
    if ((protect & 0xFF) == PAGE_NOACCESS)
        return false;

    const DWORD baseProtect = protect & 0xFF;
    if (baseProtect != PAGE_READONLY &&
        baseProtect != PAGE_READWRITE &&
        baseProtect != PAGE_WRITECOPY &&
        baseProtect != PAGE_EXECUTE_READ &&
        baseProtect != PAGE_EXECUTE_READWRITE &&
        baseProtect != PAGE_EXECUTE_WRITECOPY)
    {
        return false;
    }

    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = start + (size ? (size - 1) : 0);
    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionStart + mbi.RegionSize - 1;
    return start >= regionStart && end <= regionEnd;
}

static void AddControllerForActor(std::vector<ControllerMapping>& outMap, void* actor)
{
    if (!actor)
        return;
    if (!IsReadableAddress(actor, 0x6C))
        return;

    const UInt8 typeID = *((UInt8*)actor + 4);
    if (typeID != 0x3B && typeID != 0x3C)
        return;

    void* process = *(void**)((UInt8*)actor + 0x68);
    if (!process)
        return;
    if (!IsReadableAddress(process, 0x13C))
        return;

    UInt32 processLevel = *(UInt32*)((UInt8*)process + 0x28);
    if (processLevel > 1)
        return;

    void* charCtrl = *(void**)((UInt8*)process + 0x138);
    if (!charCtrl)
        return;

    UInt32 refID = ReadRefID(actor);
    if (!refID)
        return;

    for (const auto& mapping : outMap)
    {
        if (mapping.charCtrl == charCtrl)
            return;
    }

    outMap.push_back({charCtrl, refID});
}

static void CollectControllersFromList(std::vector<ControllerMapping>& outMap, UInt32 listOffset)
{
    if (!g_actorProcessManager)
        return;

    ListNode* node = (ListNode*)((UInt8*)g_actorProcessManager + listOffset);
    UInt32 guard = 0;
    while (node && guard++ < 4096)
    {
        if (!IsReadableAddress(node, sizeof(ListNode)))
            break;

        void* actor = node->item;
        ListNode* next = node->next;

        if (actor)
            AddControllerForActor(outMap, actor);

        node = next;
    }
}

//main thread: rebuild charCtrl->refID lookup table from ActorProcessManager lists
//AI hooks read this under lock to resolve charCtrl to a stable refID
static void RebuildControllerMap()
{
    void* player = g_thePlayer ? *g_thePlayer : nullptr;
    if (!player)
    {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);
        OnJumpLandHandler::g_controllerMap.clear();
        return;
    }

    std::vector<ControllerMapping> newMap;
    newMap.reserve(64);

    //ActorProcessManager::middleHighActors at +0x00, highActors at +0x5C
    CollectControllersFromList(newMap, 0x00);
    CollectControllersFromList(newMap, 0x5C);

    //explicit player fallback in case list state is transient
    AddControllerForActor(newMap, player);

    ScopedLock lock(&OnJumpLandHandler::g_stateLock);
    OnJumpLandHandler::g_controllerMap = std::move(newMap);
}

//called from AI hooks, lock must already be held
static UInt32 LookupRefIDFromController(void* charCtrl)
{
    for (const auto& m : OnJumpLandHandler::g_controllerMap)
        if (m.charCtrl == charCtrl) return m.refID;
    return 0;
}

static bool QueuePendingEventLocked(UInt8 eventType, UInt32 actorRefID, float preClearFallTime)
{
    if (!actorRefID)
        return false;

    if (OnJumpLandHandler::g_pendingEvents.size() >= kMaxPendingEvents)
        return false;

    OnJumpLandHandler::g_pendingEvents.push_back({eventType, actorRefID, preClearFallTime});
    return true;
}

static bool PassesActorFilter(void* actor, TESForm* filter)
{
    if (!filter) return true;
    if ((TESForm*)actor == filter) return true;

    TESForm* baseForm = ReadBaseForm(actor);
    if (baseForm == filter) return true;

    return false;
}

typedef void (__thiscall *StateUpdateVelocity_t)(void* state, void* charCtrl);
static StateUpdateVelocity_t s_originalJumpingUpdateVelocity = nullptr;
static StateUpdateVelocity_t s_originalInAirUpdateVelocity = nullptr;

static void __fastcall Hook_bhkCharacterStateJumping_UpdateVelocity(void* state, void* edx, void* charCtrl)
{
    if (s_originalJumpingUpdateVelocity)
        s_originalJumpingUpdateVelocity(state, charCtrl);

    if (!charCtrl || OnJumpLandHandler::g_stateLockInit != 2)
        return;

    ScopedLock lock(&OnJumpLandHandler::g_stateLock);

    if (OnJumpLandHandler::g_jumpCallbacks.empty())
        return;

    auto& active = OnJumpLandHandler::g_activeJumpControllers;
    if (std::find(active.begin(), active.end(), charCtrl) != active.end())
        return;

    UInt32 refID = LookupRefIDFromController(charCtrl);
    if (!refID)
        return;

    active.push_back(charCtrl);
    QueuePendingEventLocked(kEvent_JumpStart, refID, 0.0f);
}

static void __fastcall Hook_bhkCharacterStateInAir_UpdateVelocity(void* state, void* edx, void* charCtrl)
{
    const float preClearFallTime = ReadControllerFallTime(charCtrl);

    if (s_originalInAirUpdateVelocity)
        s_originalInAirUpdateVelocity(state, charCtrl);

    if (!charCtrl || OnJumpLandHandler::g_stateLockInit != 2)
        return;

    const UInt32 newState = ReadControllerState(charCtrl);
    if (newState == 0xFFFFFFFF)
        return;

    if (newState == 0 || (newState & 0x2) == 0)
    {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);

        auto& active = OnJumpLandHandler::g_activeJumpControllers;
        auto it = std::find(active.begin(), active.end(), charCtrl);
        if (it != active.end())
            active.erase(it);

        if (OnJumpLandHandler::g_landedCallbacks.empty())
            return;

        UInt32 refID = LookupRefIDFromController(charCtrl);
        if (!refID)
            return;

        QueuePendingEventLocked(kEvent_Landed, refID, preClearFallTime);
    }
}

static bool InstallHooks()
{
    if (OnJumpLandHandler::g_hooksInstalled)
        return true;

    const UInt32 jumpSlot = kVtbl_bhkCharacterStateJumping + (kVFuncIdx_UpdateVelocity * 4);
    const UInt32 inAirSlot = kVtbl_bhkCharacterStateInAir + (kVFuncIdx_UpdateVelocity * 4);

    s_originalJumpingUpdateVelocity = reinterpret_cast<StateUpdateVelocity_t>(*(UInt32*)jumpSlot);
    s_originalInAirUpdateVelocity = reinterpret_cast<StateUpdateVelocity_t>(*(UInt32*)inAirSlot);

    if (!s_originalJumpingUpdateVelocity || !s_originalInAirUpdateVelocity)
    {
        return false;
    }

    SafeWrite::Write32(jumpSlot, (UInt32)Hook_bhkCharacterStateJumping_UpdateVelocity);
    SafeWrite::Write32(inAirSlot, (UInt32)Hook_bhkCharacterStateInAir_UpdateVelocity);

    OnJumpLandHandler::g_hooksInstalled = true;
    return true;
}

static void RefreshHooksIfOverwritten()
{
    if (!OnJumpLandHandler::g_hooksInstalled)
        return;

    const UInt32 jumpSlot = kVtbl_bhkCharacterStateJumping + (kVFuncIdx_UpdateVelocity * 4);
    const UInt32 inAirSlot = kVtbl_bhkCharacterStateInAir + (kVFuncIdx_UpdateVelocity * 4);

    StateUpdateVelocity_t currentJump = reinterpret_cast<StateUpdateVelocity_t>(*(UInt32*)jumpSlot);
    StateUpdateVelocity_t currentInAir = reinterpret_cast<StateUpdateVelocity_t>(*(UInt32*)inAirSlot);

    if (currentJump != reinterpret_cast<StateUpdateVelocity_t>(Hook_bhkCharacterStateJumping_UpdateVelocity))
    {
        if (currentJump)
            s_originalJumpingUpdateVelocity = currentJump;
        SafeWrite::Write32(jumpSlot, (UInt32)Hook_bhkCharacterStateJumping_UpdateVelocity);
    }

    if (currentInAir != reinterpret_cast<StateUpdateVelocity_t>(Hook_bhkCharacterStateInAir_UpdateVelocity))
    {
        if (currentInAir)
            s_originalInAirUpdateVelocity = currentInAir;
        SafeWrite::Write32(inAirSlot, (UInt32)Hook_bhkCharacterStateInAir_UpdateVelocity);
    }
}

static UInt32 GetFormID(TESForm* form)
{
    return form ? *(UInt32*)((UInt8*)form + kOffset_RefID) : 0;
}

static bool AddCallback(std::vector<CallbackEntry>& callbacks, Script* callback, TESForm* actorFilter)
{
    if (!callback) return false;

    UInt32 formID = GetFormID((TESForm*)callback);
    if (!formID) return false;

    EnsureStateLockInitialized();
    ScopedLock lock(&OnJumpLandHandler::g_stateLock);

    for (const auto& entry : callbacks)
    {
        if (entry.callbackFormID == formID && entry.actorFilter == actorFilter)
        {
            return false;
        }
    }

    if (g_CaptureLambdaVars)
        g_CaptureLambdaVars(callback);

    callbacks.push_back({callback, actorFilter, formID});
    return true;
}

static bool RemoveCallback(std::vector<CallbackEntry>& callbacks, Script* callback, TESForm* actorFilter)
{
    if (!callback) return false;

    EnsureStateLockInitialized();
    ScopedLock lock(&OnJumpLandHandler::g_stateLock);

    for (auto it = callbacks.begin(); it != callbacks.end(); ++it)
    {
        if (it->callback == callback && it->actorFilter == actorFilter)
        {
            if (g_UncaptureLambdaVars)
                g_UncaptureLambdaVars(it->callback);
            callbacks.erase(it);
            return true;
        }
    }

    return false;
}

static ParamInfo kParams_SetJumpLandEventHandler[3] = {
    {"callback",    kParamType_AnyForm, 0},
    {"addRemove",   kParamType_Integer, 0},
    {"actorFilter", kParamType_AnyForm, 1},
};

DEFINE_COMMAND_PLUGIN(SetOnActorLandedEventHandler,
    "Registers callback for actor landing. Callback: (actor, preClearFallTimeElapsed)",
    0, 3, kParams_SetJumpLandEventHandler);

DEFINE_COMMAND_PLUGIN(SetOnJumpStartEventHandler,
    "Registers callback for actor jump start. Callback: (actor)",
    0, 3, kParams_SetJumpLandEventHandler);

bool Cmd_SetOnActorLandedEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;
    TESForm* actorFilter = nullptr;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove,
            &actorFilter))
    {
        return true;
    }

    if (!callbackForm || *((UInt8*)callbackForm + 4) != kFormType_Script)
        return true;

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove)
    {
        if (AddCallback(OnJumpLandHandler::g_landedCallbacks, callback, actorFilter))
            *result = 1;
    }
    else
    {
        if (RemoveCallback(OnJumpLandHandler::g_landedCallbacks, callback, actorFilter))
            *result = 1;
    }

    return true;
}

bool Cmd_SetOnJumpStartEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;
    TESForm* actorFilter = nullptr;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove,
            &actorFilter))
    {
        return true;
    }

    if (!callbackForm || *((UInt8*)callbackForm + 4) != kFormType_Script)
        return true;

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove)
    {
        if (AddCallback(OnJumpLandHandler::g_jumpCallbacks, callback, actorFilter))
            *result = 1;
    }
    else
    {
        if (RemoveCallback(OnJumpLandHandler::g_jumpCallbacks, callback, actorFilter))
            *result = 1;
    }

    return true;
}

void OJLH_Update()
{
    if (OnJumpLandHandler::g_stateLockInit != 2 || !g_ojlhScript)
        return;

    RefreshHooksIfOverwritten();

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnJumpLandHandler::g_mainThreadId)
        OnJumpLandHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnJumpLandHandler::g_mainThreadId)
        return;

    std::vector<QueuedEvent> queuedEvents;
    std::vector<CallbackEntry> landedSnapshot;
    std::vector<CallbackEntry> jumpSnapshot;
    std::vector<Script*> badLandedCallbacks;
    std::vector<Script*> badJumpCallbacks;

    {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);

        bool hasCallbacks = !OnJumpLandHandler::g_jumpCallbacks.empty()
                         || !OnJumpLandHandler::g_landedCallbacks.empty();
        if (!hasCallbacks)
        {
            OnJumpLandHandler::g_pendingEvents.clear();
            OnJumpLandHandler::g_controllerMap.clear();
            return;
        }

        queuedEvents.swap(OnJumpLandHandler::g_pendingEvents);
        landedSnapshot = OnJumpLandHandler::g_landedCallbacks;
        jumpSnapshot = OnJumpLandHandler::g_jumpCallbacks;
    }

    RebuildControllerMap();

    for (const auto& evt : queuedEvents)
    {
        TESForm* actorForm = LookupFormByID_Local(evt.actorRefID);
        if (!actorForm)
            continue;
        void* actor = actorForm;

        if (evt.eventType == kEvent_Landed)
        {
            for (const auto& cb : landedSnapshot)
            {
                if (!cb.callback || !cb.callbackFormID) continue;
                if (!PassesActorFilter(actor, cb.actorFilter)) continue;

                TESForm* resolved = LookupFormByID_Local(cb.callbackFormID);
                if (!resolved || resolved != (TESForm*)cb.callback)
                {
                    badLandedCallbacks.push_back(cb.callback);
                    continue;
                }

                UInt32 fallBits;
                memcpy(&fallBits, &evt.preClearFallTime, sizeof(fallBits));
                g_ojlhScript->CallFunctionAlt(
                    cb.callback,
                    nullptr,
                    2,
                    reinterpret_cast<TESForm*>(actor),
                    fallBits);
            }
        }
        else if (evt.eventType == kEvent_JumpStart)
        {
            for (const auto& cb : jumpSnapshot)
            {
                if (!cb.callback || !cb.callbackFormID) continue;
                if (!PassesActorFilter(actor, cb.actorFilter)) continue;

                TESForm* resolved = LookupFormByID_Local(cb.callbackFormID);
                if (!resolved || resolved != (TESForm*)cb.callback)
                {
                    badJumpCallbacks.push_back(cb.callback);
                    continue;
                }

                g_ojlhScript->CallFunctionAlt(
                    cb.callback,
                    nullptr,
                    1,
                    reinterpret_cast<TESForm*>(actor));
            }
        }
    }

    if (!badLandedCallbacks.empty() || !badJumpCallbacks.empty())
    {
        ScopedLock lock(&OnJumpLandHandler::g_stateLock);

        auto isBad = [](const std::vector<Script*>& badList, const CallbackEntry& entry) -> bool
        {
            return std::find(badList.begin(), badList.end(), entry.callback) != badList.end();
        };

        if (!badLandedCallbacks.empty())
        {
            if (g_UncaptureLambdaVars)
                for (auto& e : OnJumpLandHandler::g_landedCallbacks)
                    if (isBad(badLandedCallbacks, e) && e.callback)
                        g_UncaptureLambdaVars(e.callback);

            OnJumpLandHandler::g_landedCallbacks.erase(
                std::remove_if(OnJumpLandHandler::g_landedCallbacks.begin(),
                               OnJumpLandHandler::g_landedCallbacks.end(),
                               [&](const CallbackEntry& entry) { return isBad(badLandedCallbacks, entry); }),
                OnJumpLandHandler::g_landedCallbacks.end());
        }

        if (!badJumpCallbacks.empty())
        {
            if (g_UncaptureLambdaVars)
                for (auto& e : OnJumpLandHandler::g_jumpCallbacks)
                    if (isBad(badJumpCallbacks, e) && e.callback)
                        g_UncaptureLambdaVars(e.callback);

            OnJumpLandHandler::g_jumpCallbacks.erase(
                std::remove_if(OnJumpLandHandler::g_jumpCallbacks.begin(),
                               OnJumpLandHandler::g_jumpCallbacks.end(),
                               [&](const CallbackEntry& entry) { return isBad(badJumpCallbacks, entry); }),
                OnJumpLandHandler::g_jumpCallbacks.end());
        }
    }
}

bool OJLH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);
    if (nvse->isEditor)
        return false;

    g_ojlhScript = reinterpret_cast<NVSEScriptInterface*>(nvse->QueryInterface(kInterface_Script));
    if (!g_ojlhScript)
    {
        return false;
    }

    g_ExtractArgsEx = g_ojlhScript->ExtractArgsEx;

    NVSEDataInterface* nvseData = reinterpret_cast<NVSEDataInterface*>(
        nvse->QueryInterface(kInterface_Data));
    if (nvseData)
    {
        g_CaptureLambdaVars = reinterpret_cast<_CaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList));
        g_UncaptureLambdaVars = reinterpret_cast<_UncaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList));
    }

    EnsureStateLockInitialized();
    OnJumpLandHandler::g_mainThreadId = GetCurrentThreadId();

    if (!InstallHooks())
        return false;

    nvse->SetOpcodeBase(0x4046);
    nvse->RegisterCommand(&kCommandInfo_SetOnActorLandedEventHandler);
    g_ojlhLandedOpcode = 0x4046;

    nvse->SetOpcodeBase(0x4047);
    nvse->RegisterCommand(&kCommandInfo_SetOnJumpStartEventHandler);
    g_ojlhJumpOpcode = 0x4047;

    return true;
}

unsigned int OJLH_GetLandedOpcode()
{
    return g_ojlhLandedOpcode;
}

unsigned int OJLH_GetJumpOpcode()
{
    return g_ojlhJumpOpcode;
}

void OJLH_ClearCallbacks()
{
    if (OnJumpLandHandler::g_stateLockInit != 2)
        return;

    ScopedLock lock(&OnJumpLandHandler::g_stateLock);

    if (g_UncaptureLambdaVars)
    {
        for (auto& entry : OnJumpLandHandler::g_landedCallbacks)
            if (entry.callback) g_UncaptureLambdaVars(entry.callback);
        for (auto& entry : OnJumpLandHandler::g_jumpCallbacks)
            if (entry.callback) g_UncaptureLambdaVars(entry.callback);
    }

    OnJumpLandHandler::g_landedCallbacks.clear();
    OnJumpLandHandler::g_jumpCallbacks.clear();
    OnJumpLandHandler::g_pendingEvents.clear();
    OnJumpLandHandler::g_activeJumpControllers.clear();
}
