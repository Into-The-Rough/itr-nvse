//hooks console open (0x71D665) and close (0x71D720) to dispatch events

#include <cstdint>
#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnConsoleHandler.h"

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using SInt32 = int32_t;

class TESForm;
class TESObjectREFR;
class Script;
class ScriptEventList;

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

    inline void WriteRelCall(UInt32 src, UInt32 dst) {
        Write8(src, 0xE8);
        Write32(src + 1, dst - src - 5);
    }
}

static FILE* g_ochLogFile = nullptr;

static void OCH_Log(const char* fmt, ...)
{
    if (!g_ochLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_ochLogFile, fmt, args);
    fprintf(g_ochLogFile, "\n");
    fflush(g_ochLogFile);
    va_end(args);
}

static PluginHandle g_ochPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_ochScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ochOpenOpcode = 0;
static UInt32 g_ochCloseOpcode = 0;

namespace OnConsoleHandler {
    std::vector<Script*> g_openCallbacks;
    std::vector<Script*> g_closeCallbacks;
    bool g_hooksInstalled = false;

    // Original function addresses (to chain calls)
    static UInt32 s_originalOpenTarget = 0;
    static UInt32 s_originalCloseTarget = 0;
}

// Hook addresses from Stewie Tweaks
constexpr UInt32 kAddr_ConsoleOpenHook = 0x71D665;
constexpr UInt32 kAddr_ConsoleCloseHook = 0x71D720;

static void DispatchConsoleOpenEvent()
{
    OCH_Log("=== CONSOLE OPEN EVENT ===");

    for (Script* callback : OnConsoleHandler::g_openCallbacks) {
        if (g_ochScript && callback) {
            // Call with no arguments
            g_ochScript->CallFunctionAlt(callback, nullptr, 0);
        }
    }
}

static void DispatchConsoleCloseEvent()
{
    OCH_Log("=== CONSOLE CLOSE EVENT ===");

    for (Script* callback : OnConsoleHandler::g_closeCallbacks) {
        if (g_ochScript && callback) {
            // Call with no arguments
            g_ochScript->CallFunctionAlt(callback, nullptr, 0);
        }
    }
}

// Hook wrappers that call original + dispatch
static void __cdecl Hook_ConsoleOpen()
{
    DispatchConsoleOpenEvent();
    // Call original function
    if (OnConsoleHandler::s_originalOpenTarget) {
        ((void(*)())OnConsoleHandler::s_originalOpenTarget)();
    }
}

static void __cdecl Hook_ConsoleClose()
{
    DispatchConsoleCloseEvent();
    // Call original function
    if (OnConsoleHandler::s_originalCloseTarget) {
        ((void(*)())OnConsoleHandler::s_originalCloseTarget)();
    }
}

static UInt32 ReadCallTarget(UInt32 callAddr) {
    return *(UInt32*)(callAddr + 1) + callAddr + 5;
}

static void InitHooks()
{
    if (OnConsoleHandler::g_hooksInstalled) return;

    // Save original targets
    OnConsoleHandler::s_originalOpenTarget = ReadCallTarget(kAddr_ConsoleOpenHook);
    OnConsoleHandler::s_originalCloseTarget = ReadCallTarget(kAddr_ConsoleCloseHook);

    // Install our hooks
    SafeWrite::WriteRelCall(kAddr_ConsoleOpenHook, (UInt32)Hook_ConsoleOpen);
    SafeWrite::WriteRelCall(kAddr_ConsoleCloseHook, (UInt32)Hook_ConsoleClose);

    OnConsoleHandler::g_hooksInstalled = true;
    OCH_Log("Hooks installed - Open: 0x%08X (orig 0x%08X), Close: 0x%08X (orig 0x%08X)",
            kAddr_ConsoleOpenHook, OnConsoleHandler::s_originalOpenTarget,
            kAddr_ConsoleCloseHook, OnConsoleHandler::s_originalCloseTarget);
}

static bool AddOpenCallback(Script* callback)
{
    if (!callback) return false;

    for (Script* s : OnConsoleHandler::g_openCallbacks) {
        if (s == callback) {
            OCH_Log("Open callback 0x%08X already registered", callback);
            return false;
        }
    }

    OnConsoleHandler::g_openCallbacks.push_back(callback);

    if (!OnConsoleHandler::g_hooksInstalled)
        InitHooks();

    OCH_Log("Added open callback: 0x%08X (total: %d)", callback, OnConsoleHandler::g_openCallbacks.size());
    return true;
}

static bool RemoveOpenCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnConsoleHandler::g_openCallbacks.begin(); it != OnConsoleHandler::g_openCallbacks.end(); ++it) {
        if (*it == callback) {
            OnConsoleHandler::g_openCallbacks.erase(it);
            OCH_Log("Removed open callback: 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static bool AddCloseCallback(Script* callback)
{
    if (!callback) return false;

    for (Script* s : OnConsoleHandler::g_closeCallbacks) {
        if (s == callback) {
            OCH_Log("Close callback 0x%08X already registered", callback);
            return false;
        }
    }

    OnConsoleHandler::g_closeCallbacks.push_back(callback);

    if (!OnConsoleHandler::g_hooksInstalled)
        InitHooks();

    OCH_Log("Added close callback: 0x%08X (total: %d)", callback, OnConsoleHandler::g_closeCallbacks.size());
    return true;
}

static bool RemoveCloseCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnConsoleHandler::g_closeCallbacks.begin(); it != OnConsoleHandler::g_closeCallbacks.end(); ++it) {
        if (*it == callback) {
            OnConsoleHandler::g_closeCallbacks.erase(it);
            OCH_Log("Removed close callback: 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static ParamInfo kParams_ConsoleHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnConsoleOpenEventHandler,
    "Registers/unregisters a callback for console open events. Callback receives no args.",
    0, 2, kParams_ConsoleHandler);

DEFINE_COMMAND_PLUGIN(SetOnConsoleCloseEventHandler,
    "Registers/unregisters a callback for console close events. Callback receives no args.",
    0, 2, kParams_ConsoleHandler);

bool Cmd_SetOnConsoleOpenEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OCH_Log("SetOnConsoleOpenEventHandler called");

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
        OCH_Log("Failed to extract args");
        return true;
    }

    OCH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OCH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OCH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddOpenCallback(callback)) {
            *result = 1;
            OCH_Log("Open callback added successfully");
        }
    } else {
        if (RemoveOpenCallback(callback)) {
            *result = 1;
            OCH_Log("Open callback removed successfully");
        }
    }

    return true;
}

bool Cmd_SetOnConsoleCloseEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OCH_Log("SetOnConsoleCloseEventHandler called");

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
        OCH_Log("Failed to extract args");
        return true;
    }

    OCH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OCH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OCH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCloseCallback(callback)) {
            *result = 1;
            OCH_Log("Close callback added successfully");
        }
    } else {
        if (RemoveCloseCallback(callback)) {
            *result = 1;
            OCH_Log("Close callback removed successfully");
        }
    }

    return true;
}

bool OCH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    // Open log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnConsoleHandler.log");
    g_ochLogFile = fopen(logPath, "w");

    OCH_Log("OnConsoleHandler module initializing...");

    g_ochPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_ochScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_ochScript) {
        OCH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_ochScript->ExtractArgsEx;
    OCH_Log("Script interface at 0x%08X", g_ochScript);

    // Register commands at opcodes 0x3B04 and 0x3B05
    nvse->SetOpcodeBase(0x3B04);
    nvse->RegisterCommand(&kCommandInfo_SetOnConsoleOpenEventHandler);
    g_ochOpenOpcode = 0x3B04;

    nvse->SetOpcodeBase(0x3B05);
    nvse->RegisterCommand(&kCommandInfo_SetOnConsoleCloseEventHandler);
    g_ochCloseOpcode = 0x3B05;

    OCH_Log("Registered SetOnConsoleOpenEventHandler at opcode 0x%04X", g_ochOpenOpcode);
    OCH_Log("Registered SetOnConsoleCloseEventHandler at opcode 0x%04X", g_ochCloseOpcode);
    OCH_Log("OnConsoleHandler module initialized successfully");

    return true;
}

unsigned int OCH_GetOpenOpcode()
{
    return g_ochOpenOpcode;
}

unsigned int OCH_GetCloseOpcode()
{
    return g_ochCloseOpcode;
}
