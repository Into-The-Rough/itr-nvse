//hooks Actor::TryDropWeapon (0x89F580) to dispatch events when actors drop weapons

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnWeaponDropHandler.h"
#include "internal/NVSEMinimal.h"

static FILE* g_owdhLogFile = nullptr;

static void OWDH_Log(const char* fmt, ...)
{
    if (!g_owdhLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_owdhLogFile, fmt, args);
    fprintf(g_owdhLogFile, "\n");
    fflush(g_owdhLogFile);
    va_end(args);
}

static PluginHandle g_owdhPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_owdhScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_owdhOpcode = 0;

struct WeaponDropCallbackEntry {
    Script* callback;

    WeaponDropCallbackEntry(Script* script) : callback(script) {}

    WeaponDropCallbackEntry(WeaponDropCallbackEntry&& other) noexcept
        : callback(other.callback)
    {
        other.callback = nullptr;
    }

    WeaponDropCallbackEntry& operator=(WeaponDropCallbackEntry&& other) noexcept {
        if (this != &other) {
            callback = other.callback;
            other.callback = nullptr;
        }
        return *this;
    }

    WeaponDropCallbackEntry(const WeaponDropCallbackEntry&) = delete;
    WeaponDropCallbackEntry& operator=(const WeaponDropCallbackEntry&) = delete;
};

namespace OnWeaponDropHandler {
    std::vector<WeaponDropCallbackEntry> g_callbacks;
    bool g_hookInstalled = false;

    // Captured data from hook
    static Actor* s_actor = nullptr;
    static TESObjectWEAP* s_weapon = nullptr;
}

// Address constants
// Actor::TryDropWeapon at 0x89F580
// void __thiscall Actor::TryDropWeapon(Actor *this)
// Prologue: push ebp; mov ebp,esp; sub esp,3Ch (6 bytes total)
constexpr UInt32 kAddr_TryDropWeapon = 0x89F580;
constexpr UInt32 kAddr_TryDropWeaponBody = 0x89F586;  // After 6-byte prologue


//offsets verified from Fallout-Real-Time-Menus headers:
// - MobileObject inherits from TESObjectREFR (0x68 bytes)
// - pCurrentProcess is first field in MobileObject, so at offset 0x68
// - BaseProcess::GetCurrentWeapon is vtable index 82 (0x148 byte offset)
// - ItemChange structure:
//   - 0x00: pExtraLists (BSSimpleList<ExtraDataList*>*)
//   - 0x04: iCountDelta (int32_t)
//   - 0x08: pObject (TESBoundObject*) <-- the weapon form

constexpr UInt32 kOffset_MobileObject_pCurrentProcess = 0x68;
constexpr UInt32 kVtableIndex_GetCurrentWeapon = 82;
constexpr UInt32 kOffset_ItemChange_pObject = 0x08;

// Helper to get weapon from actor's current process via virtual call
static TESObjectWEAP* GetActorCurrentWeapon(Actor* actor)
{
    if (!actor) return nullptr;

    // Get pCurrentProcess at offset 0x68
    UInt32 pProcess = *(UInt32*)((UInt8*)actor + kOffset_MobileObject_pCurrentProcess);
    if (!pProcess) return nullptr;

    // Call BaseProcess::GetCurrentWeapon() via vtable
    // vtable is at offset 0 of the object, function pointer at index 82
    UInt32 vtable = *(UInt32*)pProcess;
    if (!vtable) return nullptr;

    // GetCurrentWeapon returns ItemChange* (thiscall, no args besides this)
    typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32 pProcess);
    GetCurrentWeapon_t GetCurrentWeapon = (GetCurrentWeapon_t)(*(UInt32*)(vtable + kVtableIndex_GetCurrentWeapon * 4));

    UInt32 itemChange = GetCurrentWeapon(pProcess);
    if (!itemChange) return nullptr;

    // ItemChange+0x08 = pObject (TESBoundObject*, the weapon)
    return (TESObjectWEAP*)(*(UInt32*)(itemChange + kOffset_ItemChange_pObject));
}

static void DispatchWeaponDropEvent()
{
    // Get weapon before it's dropped
    OnWeaponDropHandler::s_weapon = GetActorCurrentWeapon(OnWeaponDropHandler::s_actor);

    OWDH_Log("=== WEAPON DROP EVENT === actor=0x%08X weapon=0x%08X",
            OnWeaponDropHandler::s_actor, OnWeaponDropHandler::s_weapon);

    if (OnWeaponDropHandler::g_callbacks.empty()) return;
    if (!OnWeaponDropHandler::s_weapon) return;  // No weapon to drop

    for (const auto& entry : OnWeaponDropHandler::g_callbacks) {
        if (g_owdhScript && entry.callback) {
            // Call the UDF with: actor, weapon
            g_owdhScript->CallFunctionAlt(
                entry.callback,
                reinterpret_cast<TESObjectREFR*>(OnWeaponDropHandler::s_actor),
                2,
                OnWeaponDropHandler::s_actor,   // arg1: actor dropping weapon
                OnWeaponDropHandler::s_weapon   // arg2: weapon being dropped
            );
        }
    }
}

// Naked hook at start of Actor::TryDropWeapon
// ecx = Actor* this
static __declspec(naked) void TryDropWeaponHook()
{
    __asm
    {
        // Save actor (ecx)
        mov     OnWeaponDropHandler::s_actor, ecx

        // Call our dispatcher (preserves all registers)
        pushad
        pushfd
        call    DispatchWeaponDropEvent
        popfd
        popad

        // Execute original prologue (6 bytes we overwrote)
        push    ebp
        mov     ebp, esp
        sub     esp, 3Ch

        // Jump to rest of original function
        mov     eax, kAddr_TryDropWeaponBody
        jmp     eax
    }
}

static void InitHook()
{
    if (OnWeaponDropHandler::g_hookInstalled) return;

    // Overwrite first 6 bytes of Actor::TryDropWeapon
    // Original: 55 8B EC 83 EC 3C = push ebp; mov ebp,esp; sub esp,3Ch
    SafeWrite::WriteRelJump(kAddr_TryDropWeapon, (UInt32)TryDropWeaponHook);
    SafeWrite::Write8(kAddr_TryDropWeapon + 5, 0x90);  // NOP the 6th byte

    OnWeaponDropHandler::g_hookInstalled = true;
    OWDH_Log("Hook installed at 0x%08X, jumps back to 0x%08X", kAddr_TryDropWeapon, kAddr_TryDropWeaponBody);
}

static bool AddCallback_Internal(Script* callback)
{
    if (!callback) return false;

    // Check if already registered
    for (const auto& entry : OnWeaponDropHandler::g_callbacks) {
        if (entry.callback == callback) {
            OWDH_Log("Callback 0x%08X already registered", callback);
            return false;
        }
    }

    OnWeaponDropHandler::g_callbacks.emplace_back(callback);

    if (!OnWeaponDropHandler::g_hookInstalled) {
        InitHook();
    }

    OWDH_Log("Added callback: 0x%08X (total: %d)", callback, OnWeaponDropHandler::g_callbacks.size());
    return true;
}

static bool RemoveCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnWeaponDropHandler::g_callbacks.begin(); it != OnWeaponDropHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback) {
            OnWeaponDropHandler::g_callbacks.erase(it);
            OWDH_Log("Removed callback: 0x%08X", callback);
            return true;
        }
    }

    return false;
}

static ParamInfo kParams_WeaponDropHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnWeaponDropEventHandler,
    "Registers/unregisters a callback for weapon drop events. Callback receives: actor, weapon",
    0, 2, kParams_WeaponDropHandler);

bool Cmd_SetOnWeaponDropEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OWDH_Log("SetOnWeaponDropEventHandler called");

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
        OWDH_Log("Failed to extract args");
        return true;
    }

    OWDH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OWDH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OWDH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback_Internal(callback)) {
            *result = 1;
            OWDH_Log("Callback added successfully");
        }
    } else {
        if (RemoveCallback_Internal(callback)) {
            *result = 1;
            OWDH_Log("Callback removed successfully");
        }
    }

    return true;
}

bool OWDH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    // Open log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnWeaponDropHandler.log");
    g_owdhLogFile = fopen(logPath, "w");

    OWDH_Log("OnWeaponDropHandler module initializing...");

    g_owdhPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_owdhScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_owdhScript) {
        OWDH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_owdhScript->ExtractArgsEx;
    OWDH_Log("Script interface at 0x%08X", g_owdhScript);

    // Register command at opcode 0x3B02
    nvse->SetOpcodeBase(0x4002);
    nvse->RegisterCommand(&kCommandInfo_SetOnWeaponDropEventHandler);
    g_owdhOpcode = 0x4002;

    OWDH_Log("Registered SetOnWeaponDropEventHandler at opcode 0x%04X", g_owdhOpcode);
    OWDH_Log("OnWeaponDropHandler module initialized successfully");

    return true;
}

unsigned int OWDH_GetOpcode()
{
    return g_owdhOpcode;
}
