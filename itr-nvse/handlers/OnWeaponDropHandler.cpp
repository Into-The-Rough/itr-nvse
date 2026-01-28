//hooks Actor::TryDropWeapon (0x89F580) to dispatch events when actors drop weapons

#include <cstdint>
#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnWeaponDropHandler.h"


class TESForm;
class TESObjectREFR;
class Actor;
class Script;
class ScriptEventList;
class TESObjectWEAP;

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

struct CommandInfo;
struct ParamInfo;

using PluginHandle = UInt32;
constexpr PluginHandle kPluginHandle_Invalid = 0xFFFFFFFF;

struct NVSEInterface {
    UInt32  nvseVersion;
    UInt32  runtimeVersion;
    UInt32  editorVersion;
    UInt32  isEditor;

    bool    (*RegisterCommand)(CommandInfo* info);
    void    (*SetOpcodeBase)(UInt32 opcode);
    void*   (*QueryInterface)(UInt32 id);
    PluginHandle (*GetPluginHandle)(void);
    bool    (*RegisterTypedCommand)(CommandInfo* info, UInt32 retnType);
    const char* (*GetRuntimeDirectory)(void);
};

enum {
    kInterface_Serialization = 0,
    kInterface_Console,
    kInterface_Messaging,
    kInterface_CommandTable,
    kInterface_StringVar,
    kInterface_ArrayVar,
    kInterface_Script,
    kInterface_Data,
};

struct NVSEArrayVarInterface {
    struct Element {
        UInt8 pad[16];
    };
};

struct NVSEScriptInterface {
    enum { kVersion = 1 };

    bool    (*CallFunction)(Script* funcScript, TESObjectREFR* callingObj,
                TESObjectREFR* container, NVSEArrayVarInterface::Element* result,
                UInt8 numArgs, ...);
    int     (*GetFunctionParams)(Script* funcScript, UInt8* paramTypesOut);
    bool    (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptDataIn,
                UInt32* scriptDataOffset, Script* scriptObj, ScriptEventList* eventList, ...);
    bool    (*ExtractFormatStringArgs)(UInt32 fmtStringPos, char* buffer,
                ParamInfo* paramInfo, void* scriptDataIn, UInt32* scriptDataOffset,
                Script* scriptObj, ScriptEventList* eventList, UInt32 maxParams, ...);
    bool    (*CallFunctionAlt)(Script* funcScript, TESObjectREFR* callingObj,
                UInt8 numArgs, ...);
};

#define COMMAND_ARGS        void* paramInfo, void* scriptData, TESObjectREFR* thisObj, \
                            UInt32 containingObj, Script* scriptObj, ScriptEventList* eventList, \
                            double* result, UInt32* opcodeOffsetPtr

#define EXTRACT_ARGS_EX     paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

using CommandExecuteFunc = bool (*)(COMMAND_ARGS);
using CommandParseFunc = bool (*)(UInt32, void*, void*, void*);
using CommandEvalFunc = bool (*)(TESObjectREFR*, void*, void*, double*);

struct ParamInfo {
    const char* typeStr;
    UInt32      typeID;
    UInt32      isOptional;
};

struct CommandInfo {
    const char*         longName;
    const char*         shortName;
    UInt32              opcode;
    const char*         helpText;
    UInt16              needsParent;
    UInt16              numParams;
    ParamInfo*          params;
    CommandExecuteFunc  execute;
    CommandParseFunc    parse;
    CommandEvalFunc     eval;
    UInt32              flags;
};

enum ParamType {
    kParamType_Integer      = 0x01,
    kParamType_AnyForm      = 0x3D,
};

enum FormType {
    kFormType_Script = 0x11,
};

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

namespace SafeWrite {
    inline void Write8(UInt32 addr, UInt8 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt8*)addr = data;
        VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
    }

    inline void Write32(UInt32 addr, UInt32 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt32*)addr = data;
        VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        Write8(src, 0xE9);
        Write32(src + 1, dst - src - 5);
    }
}

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
