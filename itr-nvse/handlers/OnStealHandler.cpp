//hooks Actor::StealAlarm (0x8BFA40) to dispatch events when items are stolen

#include <vector>
#include <cstring>
#include <cstdio>
#include <Windows.h>

#include "OnStealHandler.h"
#include "internal/NVSEMinimal.h"

static FILE* g_oshLogFile = nullptr;

static void OSH_Log(const char* fmt, ...)
{
    if (!g_oshLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_oshLogFile, fmt, args);
    fprintf(g_oshLogFile, "\n");
    fflush(g_oshLogFile);
    va_end(args);
}

static PluginHandle g_oshPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_oshScript = nullptr;
static _CaptureLambdaVars g_CaptureLambdaVars = nullptr;
static _UncaptureLambdaVars g_UncaptureLambdaVars = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_oshOpcode = 0;

struct StealCallbackEntry {
    Script* callback;

    StealCallbackEntry(Script* script) : callback(script) {}

    StealCallbackEntry(StealCallbackEntry&& other) noexcept
        : callback(other.callback)
    {
        other.callback = nullptr;
    }

    StealCallbackEntry& operator=(StealCallbackEntry&& other) noexcept {
        if (this != &other) {
            callback = other.callback;
            other.callback = nullptr;
        }
        return *this;
    }

    StealCallbackEntry(const StealCallbackEntry&) = delete;
    StealCallbackEntry& operator=(const StealCallbackEntry&) = delete;
};

namespace OnStealHandler {
    std::vector<StealCallbackEntry> g_callbacks;
    bool g_hookInstalled = false;

    // Stolen item info - captured by hook, used by callback dispatcher
    static Actor* s_thief = nullptr;
    static TESObjectREFR* s_target = nullptr;
    static TESForm* s_item = nullptr;
    static SInt32 s_quantity = 0;
    static SInt32 s_value = 0;
    static TESObjectREFR* s_owner = nullptr;
}

// Address constants
// Actor::StealAlarm at 0x8BFA40
// void __thiscall Actor::StealAlarm(Actor *this, TESObjectREFR *target, TESForm *item, int quantity, int value, TESObjectREFR *owner)
// Prologue bytes: 55 8B EC 6A FF 68 xx xx xx xx (push ebp; mov ebp,esp; push -1; push offset)
// We overwrite 5 bytes with jmp, so we need to restore: push ebp; mov ebp,esp; push -1
constexpr UInt32 kAddr_StealAlarm = 0x8BFA40;
constexpr UInt32 kAddr_StealAlarmBody = 0x8BFA45;  // After our 5-byte jmp (points to 'push offset')

static void DispatchStealEvent()
{
    OSH_Log("=== STEAL EVENT === thief=0x%08X target=0x%08X item=0x%08X qty=%d owner=0x%08X",
            OnStealHandler::s_thief, OnStealHandler::s_target, OnStealHandler::s_item,
            OnStealHandler::s_quantity, OnStealHandler::s_owner);

    if (OnStealHandler::g_callbacks.empty()) return;

    for (const auto& entry : OnStealHandler::g_callbacks) {
        if (g_oshScript && entry.callback) {
            // Call the UDF with: thief, target, item, owner, quantity
            g_oshScript->CallFunctionAlt(
                entry.callback,
                reinterpret_cast<TESObjectREFR*>(OnStealHandler::s_thief),
                5,
                OnStealHandler::s_thief,    // arg1: thief (Actor*)
                OnStealHandler::s_target,   // arg2: target container/ref
                OnStealHandler::s_item,     // arg3: item stolen (TESForm*)
                OnStealHandler::s_owner,    // arg4: owner
                OnStealHandler::s_quantity  // arg5: quantity
            );
        }
    }
}

// Naked hook at start of Actor::StealAlarm
// Stack layout on entry (after call instruction):
//   [esp+0]  = return address
//   [esp+4]  = TESObjectREFR* target
//   [esp+8]  = TESForm* item
//   [esp+C]  = int quantity
//   [esp+10] = int value
//   [esp+14] = TESObjectREFR* owner
//   ecx      = Actor* this (thief)
static __declspec(naked) void StealAlarmHook()
{
    __asm
    {
        // Save thief (ecx)
        mov     OnStealHandler::s_thief, ecx

        // Save all parameters from stack
        mov     eax, [esp+4]
        mov     OnStealHandler::s_target, eax

        mov     eax, [esp+8]
        mov     OnStealHandler::s_item, eax

        mov     eax, [esp+0Ch]
        mov     OnStealHandler::s_quantity, eax

        mov     eax, [esp+10h]
        mov     OnStealHandler::s_value, eax

        mov     eax, [esp+14h]
        mov     OnStealHandler::s_owner, eax

        // Call our dispatcher (preserves all registers)
        pushad
        pushfd
        call    DispatchStealEvent
        popfd
        popad

        // Execute original prologue (5 bytes we overwrote)
        push    ebp
        mov     ebp, esp
        push    0FFFFFFFFh   // push -1

        // Jump to rest of original function (0x8BFA45 = push offset)
        mov     eax, kAddr_StealAlarmBody
        jmp     eax
    }
}

static void InitHook()
{
    if (OnStealHandler::g_hookInstalled) return;

    // Overwrite first 5 bytes of Actor::StealAlarm with jmp
    // Original: 55 8B EC 6A FF = push ebp; mov ebp,esp; push -1
    // We restore these in our hook before jumping to 0x8BFA45
    SafeWrite::WriteRelJump(kAddr_StealAlarm, (UInt32)StealAlarmHook);

    OnStealHandler::g_hookInstalled = true;
    OSH_Log("Hook installed at 0x%08X, jumps back to 0x%08X", kAddr_StealAlarm, kAddr_StealAlarmBody);
}

static bool AddCallback_Internal(Script* callback)
{
    if (!callback) return false;

    // Check if already registered
    for (const auto& entry : OnStealHandler::g_callbacks) {
        if (entry.callback == callback) {
            OSH_Log("Callback 0x%08X already registered", callback);
            return false;
        }
    }

    // Note: Lambda capture deferred - calling CaptureLambdaVars during command
    // execution can crash. The lambda should remain valid as long as the script
    // that created it is loaded.

    OnStealHandler::g_callbacks.emplace_back(callback);

    if (!OnStealHandler::g_hookInstalled) {
        InitHook();
    }

    OSH_Log("Added callback: 0x%08X (total: %d)", callback, OnStealHandler::g_callbacks.size());
    return true;
}

static bool RemoveCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnStealHandler::g_callbacks.begin(); it != OnStealHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback) {
            OnStealHandler::g_callbacks.erase(it);
            OSH_Log("Removed callback: 0x%08X", callback);
            return true;
        }
    }

    return false;
}

static ParamInfo kParams_StealHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnStealEventHandler,
    "Registers/unregisters a callback for steal events. Callback receives: thief, target, item, owner, quantity",
    0, 2, kParams_StealHandler);

bool Cmd_SetOnStealEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OSH_Log("SetOnStealEventHandler called");

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
        OSH_Log("Failed to extract args");
        return true;
    }

    OSH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OSH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OSH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback_Internal(callback)) {
            *result = 1;
            OSH_Log("Callback added successfully");
        }
    } else {
        if (RemoveCallback_Internal(callback)) {
            *result = 1;
            OSH_Log("Callback removed successfully");
        }
    }

    return true;
}

bool OSH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    // Open log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnStealHandler.log");
    //g_oshLogFile = fopen(logPath, "w"); //disabled for release

    OSH_Log("OnStealHandler module initializing...");

    g_oshPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_oshScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_oshScript) {
        OSH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_oshScript->ExtractArgsEx;
    OSH_Log("Script interface at 0x%08X", g_oshScript);

    // Get data interface for lambda capture
    NVSEDataInterface* nvseData = reinterpret_cast<NVSEDataInterface*>(
        nvse->QueryInterface(kInterface_Data));

    if (nvseData) {
        g_CaptureLambdaVars = reinterpret_cast<_CaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList));
        g_UncaptureLambdaVars = reinterpret_cast<_UncaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList));
        OSH_Log("Lambda capture: save=0x%08X unsave=0x%08X",
                g_CaptureLambdaVars, g_UncaptureLambdaVars);
    }

    // Register command at opcode 0x3B01 (after DialogueTextFilter at 0x3B00)
    nvse->SetOpcodeBase(0x4001);
    nvse->RegisterCommand(&kCommandInfo_SetOnStealEventHandler);
    g_oshOpcode = 0x4001;

    OSH_Log("Registered SetOnStealEventHandler at opcode 0x%04X", g_oshOpcode);
    OSH_Log("OnStealHandler module initialized successfully");

    return true;
}

unsigned int OSH_GetOpcode()
{
    return g_oshOpcode;
}

void OSH_ClearCallbacks()
{
    OnStealHandler::g_callbacks.clear();
    OSH_Log("Callbacks cleared on game load");
}
