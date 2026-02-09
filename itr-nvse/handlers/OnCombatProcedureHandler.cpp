//hooks CombatController::SetActionProcedure and SetMovementProcedure to fire events

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnCombatProcedureHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

static FILE* g_ocphLogFile = nullptr;

static void OCPH_Log(const char* fmt, ...)
{
    if (!g_ocphLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_ocphLogFile, fmt, args);
    fprintf(g_ocphLogFile, "\n");
    fflush(g_ocphLogFile);
    va_end(args);
}

static PluginHandle g_ocphPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_ocphScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ocphOpcode = 0;

constexpr UInt32 kAddr_SetActionProcedure = 0x980110;
constexpr UInt32 kAddr_SetMovementProcedure = 0x9801B0;
constexpr UInt32 kAddr_GetPackageOwner = 0x97AE90;
constexpr UInt32 kAddr_GetBaseForm = 0x6F2070;

struct CallbackEntry {
    Script* callback;
    TESForm* actorFilter;
    SInt32 procFilter;
};

struct QueuedCombatEvent {
    Actor* actor;
    UInt32 procType;
    bool isActionProcedure;
};

class ScopedLock {
    CRITICAL_SECTION* cs;
public:
    explicit ScopedLock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
    ~ScopedLock() { LeaveCriticalSection(cs); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};

namespace OnCombatProcedureHandler {
    std::vector<CallbackEntry> g_callbacks;
    std::vector<QueuedCombatEvent> g_pendingEvents;
    bool g_hookInstalled = false;
    CRITICAL_SECTION g_queueLock;
    bool g_lockInitialized = false;
    static UInt32 s_trampolineActionAddr = 0;
    static UInt32 s_trampolineMovementAddr = 0;
}

static Detours::JumpDetour s_actionDetour;
static Detours::JumpDetour s_movementDetour;

static Actor* GetPackageOwner(void* combatController)
{
    if (!combatController) return nullptr;
    typedef Actor* (__thiscall *GetPackageOwner_t)(void*);
    return ((GetPackageOwner_t)kAddr_GetPackageOwner)(combatController);
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

    OCPH_Log("Vtables initialized:");
    for (int i = 0; i <= 12; i++) {
        OCPH_Log("  Type %d: 0x%08X", i, s_vtableMap[i]);
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

    OCPH_Log("Unknown procedure vtable: 0x%08X", vtable);
    return (UInt32)-1;
}

static TESForm* GetActorBaseForm(Actor* actor)
{
    if (!actor) return nullptr;
    typedef TESForm* (__thiscall *GetBaseForm_t)(void*);
    return ((GetBaseForm_t)kAddr_GetBaseForm)(actor);
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
    if (!OnCombatProcedureHandler::g_lockInitialized) return;

    Actor* actor = GetPackageOwner(combatController);
    if (!actor) return;

    UInt32 procType = GetProcedureType(procedure);

    // Prevent unbounded growth during heavy AI activity.
    constexpr size_t kMaxQueuedEvents = 256;
    {
        ScopedLock lock(&OnCombatProcedureHandler::g_queueLock);
        if (OnCombatProcedureHandler::g_pendingEvents.size() >= kMaxQueuedEvents) {
            return;
        }
        OnCombatProcedureHandler::g_pendingEvents.push_back({actor, procType, isActionProcedure != 0});
    }
}

void OCPH_Update()
{
    if (!OnCombatProcedureHandler::g_lockInitialized) return;
    if (!g_ocphScript || OnCombatProcedureHandler::g_callbacks.empty()) return;

    std::vector<QueuedCombatEvent> queuedEvents;
    {
        ScopedLock lock(&OnCombatProcedureHandler::g_queueLock);
        queuedEvents.swap(OnCombatProcedureHandler::g_pendingEvents);
    }

    for (const QueuedCombatEvent& evt : queuedEvents) {
        if (!evt.actor) continue;

        OCPH_Log("DispatchCombatProcedureEvent: actor=0x%08X procType=%d isAction=%d",
                 evt.actor, evt.procType, evt.isActionProcedure ? 1 : 0);

        for (const CallbackEntry& entry : OnCombatProcedureHandler::g_callbacks) {
            if (!entry.callback) continue;
            if (!PassesActorFilter(evt.actor, entry.actorFilter)) continue;
            if (!PassesProcFilter(evt.procType, entry.procFilter)) continue;

            g_ocphScript->CallFunctionAlt(
                entry.callback,
                reinterpret_cast<TESObjectREFR*>(evt.actor),
                3,
                evt.actor,
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

    OCPH_Log("InitHooks: Installing combat procedure hooks...");

    if (!s_actionDetour.WriteRelJump(kAddr_SetActionProcedure, Hook_SetActionProcedure, 6)) {
        OCPH_Log("ERROR: Failed to install action hook");
        return;
    }
    OnCombatProcedureHandler::s_trampolineActionAddr = s_actionDetour.GetOverwrittenAddr();
    if (!OnCombatProcedureHandler::s_trampolineActionAddr) {
        OCPH_Log("ERROR: Action hook installed but trampoline missing; rolling back");
        s_actionDetour.Remove();
        return;
    }

    if (!s_movementDetour.WriteRelJump(kAddr_SetMovementProcedure, Hook_SetMovementProcedure, 6)) {
        OCPH_Log("ERROR: Failed to install movement hook; rolling back action hook");
        s_actionDetour.Remove();
        OnCombatProcedureHandler::s_trampolineActionAddr = 0;
        return;
    }
    OnCombatProcedureHandler::s_trampolineMovementAddr = s_movementDetour.GetOverwrittenAddr();
    if (!OnCombatProcedureHandler::s_trampolineMovementAddr) {
        OCPH_Log("ERROR: Movement hook installed but trampoline missing; rolling back both hooks");
        s_movementDetour.Remove();
        s_actionDetour.Remove();
        OnCombatProcedureHandler::s_trampolineActionAddr = 0;
        OnCombatProcedureHandler::s_trampolineMovementAddr = 0;
        return;
    }

    OnCombatProcedureHandler::g_hookInstalled = true;
    OCPH_Log("InitHooks: Hooks installed successfully");
}

static bool AddCallback(Script* callback, TESForm* actorFilter, SInt32 procFilter)
{
    OCPH_Log("AddCallback: callback=0x%08X actorFilter=0x%08X procFilter=%d",
             callback, actorFilter, procFilter);

    if (!callback) return false;

    for (const CallbackEntry& entry : OnCombatProcedureHandler::g_callbacks) {
        if (entry.callback == callback &&
            entry.actorFilter == actorFilter &&
            entry.procFilter == procFilter) {
            OCPH_Log("AddCallback: Duplicate entry, skipping");
            return false;
        }
    }

    OnCombatProcedureHandler::g_callbacks.push_back({callback, actorFilter, procFilter});

    if (!OnCombatProcedureHandler::g_hookInstalled) {
        InitHooks();
    }

    OCPH_Log("AddCallback: Added (total: %d)", (int)OnCombatProcedureHandler::g_callbacks.size());
    return true;
}

static bool RemoveCallback(Script* callback, TESForm* actorFilter, SInt32 procFilter)
{
    if (!callback) return false;

    for (auto it = OnCombatProcedureHandler::g_callbacks.begin();
         it != OnCombatProcedureHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback &&
            it->actorFilter == actorFilter &&
            it->procFilter == procFilter) {
            OnCombatProcedureHandler::g_callbacks.erase(it);
            OCPH_Log("RemoveCallback: Removed callback 0x%08X", callback);
            return true;
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
    OCPH_Log("SetOnCombatProcedureStartEventHandler called");

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
        OCPH_Log("Failed to extract args");
        return true;
    }

    OCPH_Log("Extracted: callback=0x%08X add=%d actorFilter=0x%08X procFilter=%d",
             callbackForm, addRemove, actorFilter, procFilter);

    if (!callbackForm) {
        OCPH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OCPH_Log("Callback is not a script (typeID: %02X)", typeID);
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

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnCombatProcedureHandler.log");
    //g_ocphLogFile = fopen(logPath, "w"); //disabled for release

    OCPH_Log("OnCombatProcedureHandler module initializing...");

    g_ocphPluginHandle = nvse->GetPluginHandle();

    g_ocphScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_ocphScript) {
        OCPH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_ocphScript->ExtractArgsEx;
    OCPH_Log("Script interface at 0x%08X", g_ocphScript);

    if (!OnCombatProcedureHandler::g_lockInitialized) {
        InitializeCriticalSection(&OnCombatProcedureHandler::g_queueLock);
        OnCombatProcedureHandler::g_lockInitialized = true;
    }

    nvse->SetOpcodeBase(0x4015);
    nvse->RegisterCommand(&kCommandInfo_SetOnCombatProcedureStartEventHandler);
    g_ocphOpcode = 0x4015;

    OCPH_Log("Registered SetOnCombatProcedureStartEventHandler at opcode 0x%04X", g_ocphOpcode);
    OCPH_Log("OnCombatProcedureHandler module initialized successfully");

    return true;
}

unsigned int OCPH_GetOpcode()
{
    return g_ocphOpcode;
}

void OCPH_ClearCallbacks()
{
    OnCombatProcedureHandler::g_callbacks.clear();
    if (OnCombatProcedureHandler::g_lockInitialized) {
        ScopedLock lock(&OnCombatProcedureHandler::g_queueLock);
        OnCombatProcedureHandler::g_pendingEvents.clear();
    }
    OCPH_Log("Callbacks cleared on game load");
}
