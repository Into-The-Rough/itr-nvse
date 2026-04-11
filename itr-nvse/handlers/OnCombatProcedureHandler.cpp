//hooks CombatController::SetActionProcedure and SetMovementProcedure to fire events

#include <vector>
#include <Windows.h>

#include "OnCombatProcedureHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"

struct QueuedCombatEvent {
    UInt32 actorRefID;
    UInt32 procType;
    bool isActionProcedure;
};

namespace OnCombatProcedureHandler {
    std::vector<QueuedCombatEvent> g_pendingEvents;
    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;
    DWORD g_mainThreadId = 0;
    static UInt32 s_trampolineActionAddr = 0;
    static UInt32 s_trampolineMovementAddr = 0;
}

static void EnsureStateLockInitialized()
{
    InitCriticalSectionOnce(&OnCombatProcedureHandler::g_stateLockInit, &OnCombatProcedureHandler::g_stateLock);
}

static Detours::JumpDetour s_actionDetour;
static Detours::JumpDetour s_movementDetour;

static Actor* GetPackageOwner(void* combatController)
{
    if (!combatController) return nullptr;
    return (Actor*)Engine::CombatController_GetPackageOwner(combatController);
}

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
}

static UInt32 s_vtableMap[13] = {0};
static bool s_vtablesInitialized = false;

static UInt32 ReadVtableFromConstructor(UInt32 ctorAddr)
{
    for (UInt32 i = 0; i < 100; i++) {
        UInt8 byte1 = *(UInt8*)(ctorAddr + i);
        UInt8 byte2 = *(UInt8*)(ctorAddr + i + 1);
        if (byte1 == 0xC7 && (byte2 == 0x00 || byte2 == 0x01)) {
            UInt32 vtable = *(UInt32*)(ctorAddr + i + 2);
            if ((vtable & 0xFFFF0000) == 0x01090000)
                return vtable;
        }
    }
    return 0;
}

static void InitVtables()
{
    if (s_vtablesInitialized) return;

    s_vtableMap[0] = ReadVtableFromConstructor(0x9D0890);
    s_vtableMap[1] = ReadVtableFromConstructor(0x9CC0C0);
    s_vtableMap[2] = ReadVtableFromConstructor(0x9CADF0);
    s_vtableMap[3] = ReadVtableFromConstructor(0x9CBAD0);
    s_vtableMap[4] = ReadVtableFromConstructor(0x9D5AD0);
    s_vtableMap[5] = ReadVtableFromConstructor(0x9DA720);
    s_vtableMap[6] = ReadVtableFromConstructor(0x9D69F0);
    s_vtableMap[7] = ReadVtableFromConstructor(0x9D2440);
    s_vtableMap[8] = ReadVtableFromConstructor(0x9CA5F0);
    s_vtableMap[9] = ReadVtableFromConstructor(0x9D6030);
    s_vtableMap[10] = ReadVtableFromConstructor(0x9D8B60);
    s_vtableMap[11] = ReadVtableFromConstructor(0x9DAA10);
    s_vtableMap[12] = ReadVtableFromConstructor(0x9D3A00);

    s_vtablesInitialized = true;
}

static UInt32 GetProcedureType(void* procedure)
{
    if (!procedure) return (UInt32)-1;
    if (!s_vtablesInitialized) InitVtables();

    UInt32 vtable = *(UInt32*)procedure;
    for (int i = 0; i <= 12; i++)
        if (s_vtableMap[i] == vtable) return i;
    return (UInt32)-1;
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
        if (!g_eventManagerInterface) return;
        if (OnCombatProcedureHandler::g_pendingEvents.size() >= kMaxQueuedEvents) return;
        OnCombatProcedureHandler::g_pendingEvents.push_back({actorRefID, procType, isActionProcedure != 0});
    }
}

namespace OnCombatProcedureHandler {
void Update()
{
    if (OnCombatProcedureHandler::g_stateLockInit != 2) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnCombatProcedureHandler::g_mainThreadId)
        OnCombatProcedureHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnCombatProcedureHandler::g_mainThreadId)
        return;

    std::vector<QueuedCombatEvent> queuedEvents;
    {
        ScopedLock lock(&OnCombatProcedureHandler::g_stateLock);
        queuedEvents.swap(OnCombatProcedureHandler::g_pendingEvents);
    }

    for (const QueuedCombatEvent& evt : queuedEvents) {
        Actor* actor = reinterpret_cast<Actor*>(Engine::LookupFormByID(evt.actorRefID));
        if (!actor) continue;

        if (g_eventManagerInterface)
            g_eventManagerInterface->DispatchEvent("ITR:OnCombatProcedure",
                reinterpret_cast<TESObjectREFR*>(actor),
                actor, (int)evt.procType, evt.isActionProcedure ? 1 : 0);
    }
}
}

static __declspec(naked) void Hook_SetActionProcedure()
{
    __asm {
        mov eax, [esp+4]                                                   //procType arg (before any push)
        pushad
        pushfd
        push 1                                                             //isActionProcedure = true
        push eax                                                           //procType
        push ecx                                                           //actor (thiscall this)
        call QueueCombatProcedureEvent
        add esp, 12                                                        //cdecl cleanup 3 dwords
        popfd
        popad
        jmp dword ptr [OnCombatProcedureHandler::s_trampolineActionAddr]   //tail into trampoline replaying stolen prologue
    }
}

static __declspec(naked) void Hook_SetMovementProcedure()
{
    __asm {
        mov eax, [esp+4]                                                     //procType arg
        pushad
        pushfd
        push 0                                                               //isActionProcedure = false
        push eax
        push ecx
        call QueueCombatProcedureEvent
        add esp, 12
        popfd
        popad
        jmp dword ptr [OnCombatProcedureHandler::s_trampolineMovementAddr]
    }
}

namespace OnCombatProcedureHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    EnsureStateLockInitialized();
    OnCombatProcedureHandler::g_mainThreadId = GetCurrentThreadId();

    //both functions: push ebp; mov ebp,esp; sub esp,10h = 6 bytes
    if (!s_actionDetour.WriteRelJump(0x980110, Hook_SetActionProcedure, 6))
        return false;
    OnCombatProcedureHandler::s_trampolineActionAddr = s_actionDetour.GetOverwrittenAddr();
    if (!OnCombatProcedureHandler::s_trampolineActionAddr) {
        s_actionDetour.Remove();
        return false;
    }

    if (!s_movementDetour.WriteRelJump(0x9801B0, Hook_SetMovementProcedure, 6)) {
        s_actionDetour.Remove();
        OnCombatProcedureHandler::s_trampolineActionAddr = 0;
        return false;
    }
    OnCombatProcedureHandler::s_trampolineMovementAddr = s_movementDetour.GetOverwrittenAddr();
    if (!OnCombatProcedureHandler::s_trampolineMovementAddr) {
        s_movementDetour.Remove();
        s_actionDetour.Remove();
        OnCombatProcedureHandler::s_trampolineActionAddr = 0;
        return false;
    }

    return true;
}
}
