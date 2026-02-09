//OnFastTravelHandler.cpp - fast travel event handler module
//hooks call at 0x93BF22 to dispatch events when player fast travels
//fixes JIP NVSE bug where eax is unreliable - we read markerRef from stack explicitly

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnFastTravelHandler.h"
#include "internal/NVSEMinimal.h"

static FILE* g_ofthLogFile = nullptr;

static void OFTH_Log(const char* fmt, ...)
{
    if (!g_ofthLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_ofthLogFile, fmt, args);
    fprintf(g_ofthLogFile, "\n");
    fflush(g_ofthLogFile);
    va_end(args);
}

static PluginHandle g_ofthPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_ofthScript = nullptr;
static _CaptureLambdaVars g_CaptureLambdaVars = nullptr;
static _UncaptureLambdaVars g_UncaptureLambdaVars = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ofthOpcode = 0;

struct FastTravelCallbackEntry {
    Script* callback;

    FastTravelCallbackEntry(Script* script) : callback(script) {}

    FastTravelCallbackEntry(FastTravelCallbackEntry&& other) noexcept
        : callback(other.callback)
    {
        other.callback = nullptr;
    }

    FastTravelCallbackEntry& operator=(FastTravelCallbackEntry&& other) noexcept {
        if (this != &other) {
            callback = other.callback;
            other.callback = nullptr;
        }
        return *this;
    }

    FastTravelCallbackEntry(const FastTravelCallbackEntry&) = delete;
    FastTravelCallbackEntry& operator=(const FastTravelCallbackEntry&) = delete;
};

namespace OnFastTravelHandler {
    std::vector<FastTravelCallbackEntry> g_callbacks;
    bool g_hookInstalled = false;

    //captured by hook, used by dispatcher
    static TESObjectREFR* s_markerRef = nullptr;
}

//hook replaces call at 0x93BF22 to PlayerCharacter::FastTravel (0x93CDF0)
//caller pushes markerRef to stack at [esp+4] before call
constexpr UInt32 kAddr_FastTravelCall = 0x93BF22;
constexpr UInt32 kAddr_FastTravelFunc = 0x93CDF0;

//for chaining - stores whatever was at the call site before us (JIP's hook or original)
static UInt32 g_previousTarget = 0;

static void DispatchFastTravelEvent()
{
    OFTH_Log("=== FAST TRAVEL EVENT === marker=0x%08X", OnFastTravelHandler::s_markerRef);

    if (OnFastTravelHandler::g_callbacks.empty()) return;

    for (const auto& entry : OnFastTravelHandler::g_callbacks) {
        if (g_ofthScript && entry.callback) {
            //call the UDF with: markerRef (destination)
            g_ofthScript->CallFunctionAlt(
                entry.callback,
                OnFastTravelHandler::s_markerRef,
                1,
                OnFastTravelHandler::s_markerRef  //arg1: destination marker
            );
        }
    }
}

//naked hook - replaces call at 0x93BF22
//stack on entry: [esp] = return addr, [esp+4] = markerRef
//ecx = PlayerCharacter (this)
static __declspec(naked) void FastTravelHook()
{
    __asm
    {
        //CRITICAL FIX: read markerRef from stack explicitly, NOT from eax
        //this fixes the JIP bug where eax is unreliable/garbage
        mov     eax, [esp+4]
        mov     OnFastTravelHandler::s_markerRef, eax

        //dispatch events (preserve all registers)
        pushad
        pushfd
        call    DispatchFastTravelEvent
        popfd
        popad

        //chain to previous hook (JIP) or original function
        jmp     g_previousTarget
    }
}

static UInt32 ReadCallTarget(UInt32 src)
{
    return *(UInt32*)(src + 1) + src + 5;
}

static void InitHook()
{
    if (OnFastTravelHandler::g_hookInstalled) return;

    //read existing target before we overwrite (could be JIP's hook or original)
    g_previousTarget = ReadCallTarget(kAddr_FastTravelCall);
    OFTH_Log("Previous call target: 0x%08X", g_previousTarget);

    //replace call at 0x93BF22 with call to our hook
    SafeWrite::WriteRelCall(kAddr_FastTravelCall, (UInt32)FastTravelHook);

    OnFastTravelHandler::g_hookInstalled = true;
    OFTH_Log("Hook installed at 0x%08X, chaining to 0x%08X", kAddr_FastTravelCall, g_previousTarget);
}

static bool AddCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (const auto& entry : OnFastTravelHandler::g_callbacks) {
        if (entry.callback == callback) {
            OFTH_Log("Callback 0x%08X already registered", callback);
            return false;
        }
    }

    OnFastTravelHandler::g_callbacks.emplace_back(callback);

    if (!OnFastTravelHandler::g_hookInstalled) {
        InitHook();
    }

    OFTH_Log("Added callback: 0x%08X (total: %d)", callback, OnFastTravelHandler::g_callbacks.size());
    return true;
}

static bool RemoveCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnFastTravelHandler::g_callbacks.begin(); it != OnFastTravelHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback) {
            OnFastTravelHandler::g_callbacks.erase(it);
            OFTH_Log("Removed callback: 0x%08X", callback);
            return true;
        }
    }

    return false;
}

static ParamInfo kParams_FastTravelHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnFastTravelHandler,
    "Registers/unregisters a callback for fast travel events. Callback receives: markerRef (destination)",
    0, 2, kParams_FastTravelHandler);

bool Cmd_SetOnFastTravelHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OFTH_Log("SetOnFastTravelHandler called");

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
        OFTH_Log("Failed to extract args");
        return true;
    }

    OFTH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OFTH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OFTH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback_Internal(callback)) {
            *result = 1;
            OFTH_Log("Callback added successfully");
        }
    } else {
        if (RemoveCallback_Internal(callback)) {
            *result = 1;
            OFTH_Log("Callback removed successfully");
        }
    }

    return true;
}

bool OFTH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnFastTravelHandler.log");
    //g_ofthLogFile = fopen(logPath, "w"); //disabled for release

    OFTH_Log("OnFastTravelHandler module initializing...");

    g_ofthPluginHandle = nvse->GetPluginHandle();

    g_ofthScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_ofthScript) {
        OFTH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_ofthScript->ExtractArgsEx;
    OFTH_Log("Script interface at 0x%08X", g_ofthScript);

    NVSEDataInterface* nvseData = reinterpret_cast<NVSEDataInterface*>(
        nvse->QueryInterface(kInterface_Data));

    if (nvseData) {
        g_CaptureLambdaVars = reinterpret_cast<_CaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList));
        g_UncaptureLambdaVars = reinterpret_cast<_UncaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList));
        OFTH_Log("Lambda capture: save=0x%08X unsave=0x%08X",
                g_CaptureLambdaVars, g_UncaptureLambdaVars);
    }

    nvse->SetOpcodeBase(0x401C);
    nvse->RegisterCommand(&kCommandInfo_SetOnFastTravelHandler);
    g_ofthOpcode = 0x401C;

    OFTH_Log("Registered SetOnFastTravelHandler at opcode 0x%04X", g_ofthOpcode);
    OFTH_Log("OnFastTravelHandler module initialized successfully");

    return true;
}

unsigned int OFTH_GetOpcode()
{
    return g_ofthOpcode;
}

void OFTH_ClearCallbacks()
{
    OnFastTravelHandler::g_callbacks.clear();
    OFTH_Log("Callbacks cleared on game load");
}
