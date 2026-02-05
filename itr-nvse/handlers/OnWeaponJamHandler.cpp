//hooks Actor::SetAnimAction call at 0x894081 in FiresWeapon when weapon jams

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnWeaponJamHandler.h"
#include "internal/NVSEMinimal.h"

static FILE* g_owjhLogFile = nullptr;

static void OWJH_Log(const char* fmt, ...)
{
    if (!g_owjhLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_owjhLogFile, fmt, args);
    fprintf(g_owjhLogFile, "\n");
    fflush(g_owjhLogFile);
    va_end(args);
}

static PluginHandle g_owjhPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_owjhScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_owjhOpcode = 0;

//static variables for hook - must be outside namespace for inline asm
static Actor* g_jamActor = nullptr;
static int g_jamAction = 0;
static void* g_jamSequence = nullptr;

namespace OnWeaponJamHandler {
    std::vector<Script*> g_callbacks;
    bool g_hookInstalled = false;

    // Original SetAnimAction function address - 0x8A73E0 (NOT 0x8A7360!)
    static UInt32 s_originalSetAnimAction = 0x8A73E0;
}

// Hook address: the call to SetAnimAction at 0x894081 in FiresWeapon
// This is specifically when action=9 (jam) is being set
constexpr UInt32 kAddr_JamSetAnimActionCall = 0x894081;

// Offsets for getting weapon from actor
constexpr UInt32 kOffset_MobileObject_pCurrentProcess = 0x68;
constexpr UInt32 kVtableIndex_GetCurrentWeapon = 82;
constexpr UInt32 kOffset_ItemChange_pObject = 0x08;

static TESObjectWEAP* GetActorCurrentWeapon(Actor* actor)
{
    OWJH_Log("      GetActorCurrentWeapon: actor=0x%08X", actor);
    if (!actor) {
        OWJH_Log("      GetActorCurrentWeapon: actor is NULL!");
        return nullptr;
    }

    UInt32 pProcess = *(UInt32*)((UInt8*)actor + kOffset_MobileObject_pCurrentProcess);
    OWJH_Log("      GetActorCurrentWeapon: pProcess=0x%08X (offset 0x%X)", pProcess, kOffset_MobileObject_pCurrentProcess);
    if (!pProcess) {
        OWJH_Log("      GetActorCurrentWeapon: pProcess is NULL!");
        return nullptr;
    }

    UInt32 vtable = *(UInt32*)pProcess;
    OWJH_Log("      GetActorCurrentWeapon: vtable=0x%08X", vtable);
    if (!vtable) {
        OWJH_Log("      GetActorCurrentWeapon: vtable is NULL!");
        return nullptr;
    }

    UInt32 funcAddr = *(UInt32*)(vtable + kVtableIndex_GetCurrentWeapon * 4);
    OWJH_Log("      GetActorCurrentWeapon: GetCurrentWeapon func at vtable[%d]=0x%08X", kVtableIndex_GetCurrentWeapon, funcAddr);

    typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32 pProcess);
    GetCurrentWeapon_t GetCurrentWeapon = (GetCurrentWeapon_t)funcAddr;

    OWJH_Log("      GetActorCurrentWeapon: Calling GetCurrentWeapon(0x%08X)...", pProcess);
    UInt32 itemChange = GetCurrentWeapon(pProcess);
    OWJH_Log("      GetActorCurrentWeapon: itemChange=0x%08X", itemChange);
    if (!itemChange) {
        OWJH_Log("      GetActorCurrentWeapon: itemChange is NULL!");
        return nullptr;
    }

    TESObjectWEAP* weapon = (TESObjectWEAP*)(*(UInt32*)(itemChange + kOffset_ItemChange_pObject));
    OWJH_Log("      GetActorCurrentWeapon: weapon=0x%08X (offset 0x%X)", weapon, kOffset_ItemChange_pObject);
    return weapon;
}

//PlayerCharacter singleton pointer
static Actor** g_thePlayer = (Actor**)0x011DEA3C;

// Called for all jams - fires for player AND NPCs
static void DispatchWeaponJamEvent()
{
    OWJH_Log(">>> DispatchWeaponJamEvent ENTERED");
    OWJH_Log("    g_jamActor=0x%08X, *g_thePlayer=0x%08X",
             g_jamActor, *g_thePlayer);

    if (!g_jamActor) {
        OWJH_Log("!!! g_jamActor is NULL, aborting");
        return;
    }

    OWJH_Log("    g_jamActor = 0x%08X", g_jamActor);

    OWJH_Log("    About to call GetActorCurrentWeapon...");
    TESObjectWEAP* weapon = GetActorCurrentWeapon(g_jamActor);
    OWJH_Log("    GetActorCurrentWeapon returned: weapon = 0x%08X", weapon);

    OWJH_Log("=== PLAYER WEAPON JAM EVENT === actor=0x%08X weapon=0x%08X",
             g_jamActor, weapon);

    OWJH_Log("    Checking callbacks count: %d", (int)OnWeaponJamHandler::g_callbacks.size());

    if (OnWeaponJamHandler::g_callbacks.empty()) {
        OWJH_Log("    No callbacks registered, returning early");
        OWJH_Log("<<< DispatchWeaponJamEvent EXITING (no callbacks)");
        return;
    }

    OWJH_Log("    About to iterate callbacks...");
    int callbackIndex = 0;
    for (Script* callback : OnWeaponJamHandler::g_callbacks) {
        OWJH_Log("    Processing callback[%d] = 0x%08X", callbackIndex, callback);
        if (g_owjhScript && callback) {
            OWJH_Log("    Calling CallFunctionAlt...");
            // Call the UDF with: actor, weapon
            g_owjhScript->CallFunctionAlt(
                callback,
                reinterpret_cast<TESObjectREFR*>(g_jamActor),
                2,
                g_jamActor,  // arg1: actor whose weapon jammed
                weapon       // arg2: weapon that jammed
            );
            OWJH_Log("    CallFunctionAlt returned successfully");
        } else {
            OWJH_Log("    !!! Skipping callback: g_owjhScript=0x%08X callback=0x%08X", g_owjhScript, callback);
        }
        callbackIndex++;
    }

    OWJH_Log("<<< DispatchWeaponJamEvent EXITING (completed)");
}

// Address for indirect jump - 0x8A73E0 (9073632 decimal), NOT 0x8A7360!
static UInt32 s_SetAnimActionAddr = 0x8A73E0;

// Full hook with dispatch - fires for ALL actors (player and NPCs)
static __declspec(naked) void Hook_SetAnimAction_Jam()
{
    __asm {
        // Save actor and dispatch for everyone
        mov g_jamActor, ecx
        pushad
        pushfd
        call DispatchWeaponJamEvent
        popfd
        popad

        // Jump to original SetAnimAction (0x8A73E0)
        jmp dword ptr [s_SetAnimActionAddr]
    }
}

static void InitHook()
{
    OWJH_Log("InitHook called, g_hookInstalled=%d", OnWeaponJamHandler::g_hookInstalled);
    if (OnWeaponJamHandler::g_hookInstalled) {
        OWJH_Log("Hook already installed, skipping");
        return;
    }

    OWJH_Log("Installing hook: src=0x%08X dst=0x%08X", kAddr_JamSetAnimActionCall, (UInt32)Hook_SetAnimAction_Jam);
    OWJH_Log("Original SetAnimAction at 0x%08X", OnWeaponJamHandler::s_originalSetAnimAction);

    // Replace the call at 0x894081 with our hook
    SafeWrite::WriteRelCall(kAddr_JamSetAnimActionCall, (UInt32)Hook_SetAnimAction_Jam);

    OnWeaponJamHandler::g_hookInstalled = true;
    OWJH_Log("Hook installed successfully at 0x%08X", kAddr_JamSetAnimActionCall);
}

static bool AddCallback(Script* callback)
{
    OWJH_Log("AddCallback called with callback=0x%08X", callback);
    if (!callback) {
        OWJH_Log("AddCallback: callback is NULL, returning false");
        return false;
    }

    OWJH_Log("AddCallback: Checking for duplicates among %d existing callbacks", (int)OnWeaponJamHandler::g_callbacks.size());
    for (Script* s : OnWeaponJamHandler::g_callbacks) {
        if (s == callback) {
            OWJH_Log("AddCallback: Callback 0x%08X already registered, returning false", callback);
            return false;
        }
    }

    OWJH_Log("AddCallback: Adding callback to vector...");
    OnWeaponJamHandler::g_callbacks.push_back(callback);
    OWJH_Log("AddCallback: Callback added, total now: %d", (int)OnWeaponJamHandler::g_callbacks.size());

    if (!OnWeaponJamHandler::g_hookInstalled) {
        OWJH_Log("AddCallback: Hook not installed yet, calling InitHook...");
        InitHook();
    }

    OWJH_Log("AddCallback: SUCCESS - Added callback 0x%08X (total: %d)", callback, (int)OnWeaponJamHandler::g_callbacks.size());
    return true;
}

static bool RemoveCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnWeaponJamHandler::g_callbacks.begin(); it != OnWeaponJamHandler::g_callbacks.end(); ++it) {
        if (*it == callback) {
            OnWeaponJamHandler::g_callbacks.erase(it);
            OWJH_Log("Removed callback: 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static ParamInfo kParams_WeaponJamHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnWeaponJamEventHandler,
    "Registers/unregisters a callback for weapon jam events. Callback receives: actor, weapon",
    0, 2, kParams_WeaponJamHandler);

bool Cmd_SetOnWeaponJamEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OWJH_Log("SetOnWeaponJamEventHandler called");

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove))
    {
        OWJH_Log("Failed to extract args");
        return true;
    }

    OWJH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OWJH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OWJH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback(callback)) {
            *result = 1;
            OWJH_Log("Callback added successfully");
        }
    } else {
        if (RemoveCallback(callback)) {
            *result = 1;
            OWJH_Log("Callback removed successfully");
        }
    }

    return true;
}

bool OWJH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    // Open log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnWeaponJamHandler.log");
    g_owjhLogFile = fopen(logPath, "w");

    OWJH_Log("OnWeaponJamHandler module initializing...");

    g_owjhPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_owjhScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_owjhScript) {
        OWJH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_owjhScript->ExtractArgsEx;
    OWJH_Log("Script interface at 0x%08X", g_owjhScript);

    // Register command at opcode 0x3B06
    nvse->SetOpcodeBase(0x4005);
    nvse->RegisterCommand(&kCommandInfo_SetOnWeaponJamEventHandler);
    g_owjhOpcode = 0x4005;

    OWJH_Log("Registered SetOnWeaponJamEventHandler at opcode 0x%04X", g_owjhOpcode);
    OWJH_Log("OnWeaponJamHandler module initialized successfully");

    return true;
}

unsigned int OWJH_GetOpcode()
{
    return g_owjhOpcode;
}

void OWJH_ClearCallbacks()
{
    OnWeaponJamHandler::g_callbacks.clear();
    OWJH_Log("Callbacks cleared on game load");
}
