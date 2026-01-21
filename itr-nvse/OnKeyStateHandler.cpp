//provides DisableKeyEx/EnableKeyEx wrapper commands that fire events

#include <cstdint>
#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnKeyStateHandler.h"

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

struct NVSEConsoleInterface {
    enum { kVersion = 1 };
    UInt32  version;
    bool    (*RunScriptLine)(const char* buf, TESObjectREFR* object);
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

static FILE* g_okshLogFile = nullptr;

static void OKSH_Log(const char* fmt, ...)
{
    if (!g_okshLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_okshLogFile, fmt, args);
    fprintf(g_okshLogFile, "\n");
    fflush(g_okshLogFile);
    va_end(args);
}

static PluginHandle g_okshPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_okshScript = nullptr;
static NVSEConsoleInterface* g_okshConsole = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_okshDisabledOpcode = 0;
static UInt32 g_okshEnabledOpcode = 0;

namespace OnKeyStateHandler {
    std::vector<Script*> g_disabledCallbacks;
    std::vector<Script*> g_enabledCallbacks;
}

static void DispatchKeyDisabledEvent(UInt32 keycode, UInt32 mask)
{
    OKSH_Log(">>> DispatchKeyDisabledEvent: keycode=%d mask=%d", keycode, mask);

    if (OnKeyStateHandler::g_disabledCallbacks.empty()) {
        OKSH_Log("    No callbacks registered");
        return;
    }

    for (Script* callback : OnKeyStateHandler::g_disabledCallbacks) {
        if (g_okshScript && callback) {
            OKSH_Log("    Calling callback 0x%08X", callback);
            g_okshScript->CallFunctionAlt(callback, nullptr, 2, keycode, mask);
        }
    }
    OKSH_Log("<<< DispatchKeyDisabledEvent done");
}

static void DispatchKeyEnabledEvent(UInt32 keycode, UInt32 mask)
{
    OKSH_Log(">>> DispatchKeyEnabledEvent: keycode=%d mask=%d", keycode, mask);

    if (OnKeyStateHandler::g_enabledCallbacks.empty()) {
        OKSH_Log("    No callbacks registered");
        return;
    }

    for (Script* callback : OnKeyStateHandler::g_enabledCallbacks) {
        if (g_okshScript && callback) {
            OKSH_Log("    Calling callback 0x%08X", callback);
            g_okshScript->CallFunctionAlt(callback, nullptr, 2, keycode, mask);
        }
    }
    OKSH_Log("<<< DispatchKeyEnabledEvent done");
}

static bool AddDisabledCallback(Script* callback)
{
    OKSH_Log("AddDisabledCallback: callback=0x%08X", callback);
    if (!callback) return false;

    for (Script* s : OnKeyStateHandler::g_disabledCallbacks) {
        if (s == callback) {
            OKSH_Log("AddDisabledCallback: Already registered");
            return false;
        }
    }

    OnKeyStateHandler::g_disabledCallbacks.push_back(callback);
    OKSH_Log("AddDisabledCallback: Added (total: %d)", (int)OnKeyStateHandler::g_disabledCallbacks.size());
    return true;
}

static bool RemoveDisabledCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnKeyStateHandler::g_disabledCallbacks.begin();
         it != OnKeyStateHandler::g_disabledCallbacks.end(); ++it) {
        if (*it == callback) {
            OnKeyStateHandler::g_disabledCallbacks.erase(it);
            OKSH_Log("RemoveDisabledCallback: Removed 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static bool AddEnabledCallback(Script* callback)
{
    OKSH_Log("AddEnabledCallback: callback=0x%08X", callback);
    if (!callback) return false;

    for (Script* s : OnKeyStateHandler::g_enabledCallbacks) {
        if (s == callback) {
            OKSH_Log("AddEnabledCallback: Already registered");
            return false;
        }
    }

    OnKeyStateHandler::g_enabledCallbacks.push_back(callback);
    OKSH_Log("AddEnabledCallback: Added (total: %d)", (int)OnKeyStateHandler::g_enabledCallbacks.size());
    return true;
}

static bool RemoveEnabledCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnKeyStateHandler::g_enabledCallbacks.begin();
         it != OnKeyStateHandler::g_enabledCallbacks.end(); ++it) {
        if (*it == callback) {
            OnKeyStateHandler::g_enabledCallbacks.erase(it);
            OKSH_Log("RemoveEnabledCallback: Removed 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static ParamInfo kParams_KeyStateHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnKeyDisabledEventHandler,
    "Registers/unregisters a callback for DisableKeyEx events. Callback receives: keycode, mask",
    0, 2, kParams_KeyStateHandler);

bool Cmd_SetOnKeyDisabledEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OKSH_Log("SetOnKeyDisabledEventHandler called");

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
        OKSH_Log("Failed to extract args");
        return true;
    }

    OKSH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OKSH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OKSH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddDisabledCallback(callback)) {
            *result = 1;
            OKSH_Log("Callback added successfully");
        }
    } else {
        if (RemoveDisabledCallback(callback)) {
            *result = 1;
            OKSH_Log("Callback removed successfully");
        }
    }

    return true;
}

DEFINE_COMMAND_PLUGIN(SetOnKeyEnabledEventHandler,
    "Registers/unregisters a callback for EnableKeyEx events. Callback receives: keycode, mask",
    0, 2, kParams_KeyStateHandler);

bool Cmd_SetOnKeyEnabledEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OKSH_Log("SetOnKeyEnabledEventHandler called");

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
        OKSH_Log("Failed to extract args");
        return true;
    }

    OKSH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm) {
        OKSH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OKSH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddEnabledCallback(callback)) {
            *result = 1;
            OKSH_Log("Callback added successfully");
        }
    } else {
        if (RemoveEnabledCallback(callback)) {
            *result = 1;
            OKSH_Log("Callback removed successfully");
        }
    }

    return true;
}

static ParamInfo kParams_KeyEx[2] = {
    {"keycode", kParamType_Integer, 0},
    {"mask",    kParamType_Integer, 1},  // optional
};

DEFINE_COMMAND_PLUGIN(DisableKeyEx,
    "Disables a key and fires OnKeyDisabled event. Args: keycode, mask (optional)",
    0, 2, kParams_KeyEx);

bool Cmd_DisableKeyEx_Execute(COMMAND_ARGS)
{
    *result = 0;
    UInt32 keycode = 0;
    UInt32 mask = 0;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &keycode,
            &mask))
    {
        OKSH_Log("DisableKeyEx: Failed to extract args");
        return true;
    }

    OKSH_Log("DisableKeyEx: keycode=%d mask=%d", keycode, mask);

    // Call original DisableKey via console
    char cmd[64];
    if (mask)
        sprintf_s(cmd, "DisableKey %d %d", keycode, mask);
    else
        sprintf_s(cmd, "DisableKey %d", keycode);

    if (g_okshConsole) {
        g_okshConsole->RunScriptLine(cmd, nullptr);
        OKSH_Log("DisableKeyEx: Executed '%s'", cmd);
    }

    // Dispatch event
    DispatchKeyDisabledEvent(keycode, mask);

    *result = 1;
    return true;
}

DEFINE_COMMAND_PLUGIN(EnableKeyEx,
    "Enables a key and fires OnKeyEnabled event. Args: keycode, mask (optional)",
    0, 2, kParams_KeyEx);

bool Cmd_EnableKeyEx_Execute(COMMAND_ARGS)
{
    *result = 0;
    UInt32 keycode = 0;
    UInt32 mask = 0;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &keycode,
            &mask))
    {
        OKSH_Log("EnableKeyEx: Failed to extract args");
        return true;
    }

    OKSH_Log("EnableKeyEx: keycode=%d mask=%d", keycode, mask);

    // Call original EnableKey via console
    char cmd[64];
    if (mask)
        sprintf_s(cmd, "EnableKey %d %d", keycode, mask);
    else
        sprintf_s(cmd, "EnableKey %d", keycode);

    if (g_okshConsole) {
        g_okshConsole->RunScriptLine(cmd, nullptr);
        OKSH_Log("EnableKeyEx: Executed '%s'", cmd);
    }

    // Dispatch event
    DispatchKeyEnabledEvent(keycode, mask);

    *result = 1;
    return true;
}

bool OKSH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    // Open log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnKeyStateHandler.log");
    g_okshLogFile = fopen(logPath, "w");

    OKSH_Log("OnKeyStateHandler module initializing...");

    g_okshPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_okshScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_okshScript) {
        OKSH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_okshScript->ExtractArgsEx;
    OKSH_Log("Script interface at 0x%08X", g_okshScript);

    // Get console interface
    g_okshConsole = reinterpret_cast<NVSEConsoleInterface*>(
        nvse->QueryInterface(kInterface_Console));

    if (!g_okshConsole) {
        OKSH_Log("ERROR: Failed to get console interface");
        return false;
    }
    OKSH_Log("Console interface at 0x%08X", g_okshConsole);

    // Register commands at opcodes 0x3B07-0x3B0A
    nvse->SetOpcodeBase(0x3B07);
    nvse->RegisterCommand(&kCommandInfo_SetOnKeyDisabledEventHandler);
    g_okshDisabledOpcode = 0x3B07;

    nvse->SetOpcodeBase(0x3B08);
    nvse->RegisterCommand(&kCommandInfo_SetOnKeyEnabledEventHandler);
    g_okshEnabledOpcode = 0x3B08;

    nvse->SetOpcodeBase(0x3B09);
    nvse->RegisterCommand(&kCommandInfo_DisableKeyEx);

    nvse->SetOpcodeBase(0x3B0A);
    nvse->RegisterCommand(&kCommandInfo_EnableKeyEx);

    OKSH_Log("Registered SetOnKeyDisabledEventHandler at opcode 0x%04X", g_okshDisabledOpcode);
    OKSH_Log("Registered SetOnKeyEnabledEventHandler at opcode 0x%04X", g_okshEnabledOpcode);
    OKSH_Log("Registered DisableKeyEx at opcode 0x3B09");
    OKSH_Log("Registered EnableKeyEx at opcode 0x3B0A");
    OKSH_Log("OnKeyStateHandler module initialized successfully");

    return true;
}

unsigned int OKSH_GetDisabledOpcode()
{
    return g_okshDisabledOpcode;
}

unsigned int OKSH_GetEnabledOpcode()
{
    return g_okshEnabledOpcode;
}
