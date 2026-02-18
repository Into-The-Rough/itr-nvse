//hooks CombatController::SetActionProcedure and SetMovementProcedure to fire events

#include <vector>
#include <Windows.h>

#include "OnCombatProcedureHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"

static PluginHandle g_ocphPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_ocphScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ocphOpcode = 0;

struct CallbackEntry {
    Script* callback;
    TESForm* actorFilter;
    SInt32 procFilter;
};

struct QueuedCombatEvent {
    UInt32 actorRefID;
    UInt32 procType;
    bool isActionProcedure;
};

namespace OnCombatProcedureHandler {
    std::vector<CallbackEntry> g_callbacks;
    std::vector<QueuedCombatEvent> g_pendingEvents;
    bool g_hookInstalled = false;
    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;
    DWORD g_mainThreadId = 0;
    UInt32 g_droppedEvents = 0;
    DWORD g_lastDropLogTick = 0;
    static UInt32 s_trampolineActionAddr = 0;
    static UInt32 s_trampolineMovementAddr = 0;
}

static void EnsureStateLockInitialized()
{
    if (OnCombatProcedureHandler::g_stateLockInit == 2) return;

    if (InterlockedCompareExchange(&OnCombatProcedureHandler::g_stateLockInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&OnCombatProcedureHandler::g_stateLock);
        InterlockedExchange(&OnCombatProcedureHandler::g_stateLockInit, 2);
        return;
    }

    while (OnCombatProcedureHandler::g_stateLockInit != 2)
        Sleep(0);
}

static Detours::JumpDetour s_actionDetour;
static Detours::JumpDetour s_movementDetour;

static Actor* GetPackageOwner(void* combatController)
{
    if (!combatController) return nullptr;
    typedef Actor* (__thiscall *GetPackageOwner_t)(void*);
    return ((GetPackageOwner_t)0x97AE90)(combatController); //GetPackageOwner
}

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
}

static TESForm* LookupFormByID(UInt32 refID)
{
    if (!refID) return nullptr;
    struct Entry { Entry* next; UInt32 key; TESForm* form; };
    UInt8* map = *(UInt8**)0x11C54C0;
    if (!map) return nullptr;
    UInt32 numBuckets = *(UInt32*)(map + 4);
    Entry** buckets = *(Entry***)(map + 8);
    if (!buckets || !numBuckets) return nullptr;
    for (Entry* e = buckets[refID % numBuckets]; e; e = e->next)
        if (e->key == refID) return e->form;
    return nullptr;
}

//vtable addresses for each procedure type (0-12)
static UInt32 s_vtableMap[13] = {0};
static bool s_vtablesInitialized = false;

//scan constructor for "mov [reg], vtable_addr" pattern
static UInt32 ReadVtableFromConstructor(UInt32 ctorAddr)
{
    for (UInt32 i = 0; i < 100; i++) {
        UInt8 byte1 = *(UInt8*)(ctorAddr + i);
        UInt8 byte2 = *(UInt8*)(ctorAddr + i + 1);
        if (byte1 == 0xC7 && (byte2 == 0x00 || byte2 == 0x01)) {
            UInt32 vtable = *(UInt32*)(ctorAddr + i + 2);
            if ((vtable & 0xFFFF0000) == 0x01090000) {
                return vtable;
            }
        }
    }
    return 0;
}

static void InitVtables()
{
    if (s_vtablesInitialized) return;

    //constructor addresses for procedure types 0-12
    s_vtableMap[0] = ReadVtableFromConstructor(0x9D0890);   //AttackRanged
    s_vtableMap[1] = ReadVtableFromConstructor(0x9CC0C0);   //AttackMelee
    s_vtableMap[2] = ReadVtableFromConstructor(0x9CADF0);   //AttackGrenade
    s_vtableMap[3] = ReadVtableFromConstructor(0x9CBAD0);   //AttackLow
    s_vtableMap[4] = ReadVtableFromConstructor(0x9D5AD0);   //Evade
    s_vtableMap[5] = ReadVtableFromConstructor(0x9DA720);   //SwitchWeapon
    s_vtableMap[6] = ReadVtableFromConstructor(0x9D69F0);   //Move
    s_vtableMap[7] = ReadVtableFromConstructor(0x9D2440);   //BeInCover
    s_vtableMap[8] = ReadVtableFromConstructor(0x9CA5F0);   //ActivateObject
    s_vtableMap[9] = ReadVtableFromConstructor(0x9D6030);   //HideFromTarget
    s_vtableMap[10] = ReadVtableFromConstructor(0x9D8B60);  //Search
    s_vtableMap[11] = ReadVtableFromConstructor(0x9DAA10);  //UseCombatItem
    s_vtableMap[12] = ReadVtableFromConstructor(0x9D3A00);  //EngageTarget

    for (int i = 0; i <= 12; i++) {
    }

    s_vtablesInitialized = true;
}

static UInt32 GetProcedureType(void* procedure)
{
    if (!procedure) return (UInt32)-1;

    if (!s_vtablesInitialized) {
        InitVtables();
    }

    UInt32 vtable = *(UInt32*)procedure;

    for (int i = 0; i <= 12; i++) {
        if (s_vtableMap[i] == vtable) {
            return i;
        }
    }

    return (UInt32)-1;
}

static TESForm* GetActorBaseForm(Actor* actor)
{
    if (!actor) return nullptr;
    typedef TESForm* (__thiscall *GetBaseForm_t)(void*);
    return ((GetBaseForm_t)0x6F2070)(actor); //GetBaseForm
}

static bool PassesActorFilter(Actor* actor, TESForm* filter)
{
    if (!filter) return true;
    if ((TESForm*)actor == filter) return true;
    TESForm* baseForm = GetActorBaseForm(actor);
    if (baseForm == filter) return true;
    return false;
}

static bool PassesProcFilter(UInt32 procType, SInt32 filter)
{
    if (filter < 0) return true;
    return (SInt32)procType == filter;
}

static void __cdecl QueueCombatProcedureEvent(void* combatController, void* procedure, UInt32 isActionProcedure)
{
    if (!combatController || !procedure) return;
    if (OnCombatProcedureHandler::g_stateLockInit != 2) return;

    Actor* actor = GetPackageOwner(combatController);
    if (!actor) return;
    UInt32 actorRefID = ReadRefID(actor);
    if (!actorRefID) return;

    UInt32 procType = GetProcedureType(procedure);

    constexpr size_t kMaxQueuedEvents = 256;
    {
        ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
        if (OnCombatProcedureHandler::g_callbacks.empty()) {
            return;
        }
        if (OnCombatProcedureHandler::g_pendingEvents.size() >= kMaxQueuedEvents) {
            ++OnCombatProcedureHandler::g_droppedEvents;
            return;
        }
        OnCombatProcedureHandler::g_pendingEvents.push_back({actorRefID, procType, isActionProcedure != 0});
    }
}

void OCPH_Update()
{
    if (OnCombatProcedureHandler::g_stateLockInit != 2) return;
    if (!g_ocphScript) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnCombatProcedureHandler::g_mainThreadId)
        OnCombatProcedureHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnCombatProcedureHandler::g_mainThreadId)
        return;

    std::vector<QueuedCombatEvent> queuedEvents;
    std::vector<CallbackEntry> callbackSnapshot;
    UInt32 droppedToLog = 0;
    DWORD now = GetTickCount();
    {
        ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
        queuedEvents.swap(OnCombatProcedureHandler::g_pendingEvents);
        callbackSnapshot = OnCombatProcedureHandler::g_callbacks;
        if (OnCombatProcedureHandler::g_droppedEvents &&
            (now - OnCombatProcedureHandler::g_lastDropLogTick) >= 5000)
        {
            droppedToLog = OnCombatProcedureHandler::g_droppedEvents;
            OnCombatProcedureHandler::g_droppedEvents = 0;
            OnCombatProcedureHandler::g_lastDropLogTick = now;
        }
    }

    if (droppedToLog)
    if (callbackSnapshot.empty()) return;

    for (const QueuedCombatEvent& evt : queuedEvents) {
        Actor* actor = reinterpret_cast<Actor*>(LookupFormByID(evt.actorRefID));
        if (!actor) continue;

        for (const CallbackEntry& entry : callbackSnapshot) {
            if (!entry.callback) continue;
            if (!PassesActorFilter(actor, entry.actorFilter)) continue;
            if (!PassesProcFilter(evt.procType, entry.procFilter)) continue;

            g_ocphScript->CallFunctionAlt(
                entry.callback,
                reinterpret_cast<TESObjectREFR*>(actor),
                3,
                actor,
                evt.procType,
                evt.isActionProcedure ? 1 : 0
            );
        }
    }
}

static __declspec(naked) void Hook_SetActionProcedure()
{
    __asm {
        mov eax, [esp+4]

        pushad
        pushfd
        push 1
        push eax
        push ecx
        call QueueCombatProcedureEvent
        add esp, 12
        popfd
        popad

        jmp dword ptr [OnCombatProcedureHandler::s_trampolineActionAddr]
    }
}

static __declspec(naked) void Hook_SetMovementProcedure()
{
    __asm {
        mov eax, [esp+4]

        pushad
        pushfd
        push 0
        push eax
        push ecx
        call QueueCombatProcedureEvent
        add esp, 12
        popfd
        popad

        jmp dword ptr [OnCombatProcedureHandler::s_trampolineMovementAddr]
    }
}

//both functions: push ebp; mov ebp,esp; sub esp,10h = 6 bytes
static void InitHooks()
{
    if (OnCombatProcedureHandler::g_hookInstalled) return;

    if (!s_actionDetour.WriteRelJump(0x980110, Hook_SetActionProcedure, 6)) { //SetActionProcedure
        return;
    }
    OnCombatProcedureHandler::s_trampolineActionAddr = s_actionDetour.GetOverwrittenAddr();
    if (!OnCombatProcedureHandler::s_trampolineActionAddr) {
        s_actionDetour.Remove();
        return;
    }

    if (!s_movementDetour.WriteRelJump(0x9801B0, Hook_SetMovementProcedure, 6)) { //SetMovementProcedure
        s_actionDetour.Remove();
        OnCombatProcedureHandler::s_trampolineActionAddr = 0;
        return;
    }
    OnCombatProcedureHandler::s_trampolineMovementAddr = s_movementDetour.GetOverwrittenAddr();
    if (!OnCombatProcedureHandler::s_trampolineMovementAddr) {
        s_movementDetour.Remove();
        s_actionDetour.Remove();
        OnCombatProcedureHandler::s_trampolineActionAddr = 0;
        OnCombatProcedureHandler::s_trampolineMovementAddr = 0;
        return;
    }

    OnCombatProcedureHandler::g_hookInstalled = true;
}

static bool AddCallback(Script* callback, TESForm* actorFilter, SInt32 procFilter)
{

    if (!callback) return false;
    EnsureStateLockInitialized();

    {
        ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
        for (const CallbackEntry& entry : OnCombatProcedureHandler::g_callbacks) {
            if (entry.callback == callback &&
                entry.actorFilter == actorFilter &&
                entry.procFilter == procFilter) {
                return false;
            }
        }

        OnCombatProcedureHandler::g_callbacks.push_back({callback, actorFilter, procFilter});
    }

    if (!OnCombatProcedureHandler::g_hookInstalled) {
        InitHooks();
    }

    int callbackCount = 0;
    {
        ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
        callbackCount = (int)OnCombatProcedureHandler::g_callbacks.size();
    }

    return true;
}

static bool RemoveCallback(Script* callback, TESForm* actorFilter, SInt32 procFilter)
{
    if (!callback) return false;
    EnsureStateLockInitialized();

    {
        ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
        for (auto it = OnCombatProcedureHandler::g_callbacks.begin();
             it != OnCombatProcedureHandler::g_callbacks.end(); ++it) {
            if (it->callback == callback &&
                it->actorFilter == actorFilter &&
                it->procFilter == procFilter) {
                OnCombatProcedureHandler::g_callbacks.erase(it);
                return true;
            }
        }
    }
    return false;
}

static ParamInfo kParams_CombatProcHandler[4] = {
    {"callback",    kParamType_AnyForm, 0},
    {"addRemove",   kParamType_Integer, 0},
    {"actorFilter", kParamType_AnyForm, 1},
    {"procFilter",  kParamType_Integer, 1},
};

DEFINE_COMMAND_PLUGIN(SetOnCombatProcedureStartEventHandler,
    "Registers callback for combat procedure start. Callback: (actor, procType, isAction)",
    0, 4, kParams_CombatProcHandler);

bool Cmd_SetOnCombatProcedureStartEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;
    TESForm* actorFilter = nullptr;
    SInt32 procFilter = -1;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove,
            &actorFilter,
            &procFilter))
    {
        return true;
    }

    if (!callbackForm) {
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback(callback, actorFilter, procFilter)) {
            *result = 1;
        }
    } else {
        if (RemoveCallback(callback, actorFilter, procFilter)) {
            *result = 1;
        }
    }

    return true;
}

bool OCPH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_ocphPluginHandle = nvse->GetPluginHandle();

    g_ocphScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_ocphScript) {
        return false;
    }

    g_ExtractArgsEx = g_ocphScript->ExtractArgsEx;

    EnsureStateLockInitialized();
    OnCombatProcedureHandler::g_mainThreadId = GetCurrentThreadId();
    OnCombatProcedureHandler::g_lastDropLogTick = GetTickCount();

    nvse->SetOpcodeBase(0x4015);
    nvse->RegisterCommand(&kCommandInfo_SetOnCombatProcedureStartEventHandler);
    g_ocphOpcode = 0x4015;

    return true;
}

unsigned int OCPH_GetOpcode()
{
    return g_ocphOpcode;
}

void OCPH_ClearCallbacks()
{
    if (OnCombatProcedureHandler::g_stateLockInit != 2) return;

    ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
    OnCombatProcedureHandler::g_callbacks.clear();
    OnCombatProcedureHandler::g_pendingEvents.clear();
    OnCombatProcedureHandler::g_droppedEvents = 0;
}
