//hooks console open (0x71D665) and close (0x71D720) to dispatch events

#include <vector>
#include <Windows.h>

#include "OnConsoleHandler.h"
#include "internal/NVSEMinimal.h"

static PluginHandle g_ochPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_ochScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ochOpenOpcode = 0;
static UInt32 g_ochCloseOpcode = 0;

namespace OnConsoleHandler {
    std::vector<Script*> g_openCallbacks;
    std::vector<Script*> g_closeCallbacks;
    bool g_hooksInstalled = false;

    static UInt32 s_originalOpenTarget = 0;
    static UInt32 s_originalCloseTarget = 0;
}

constexpr UInt32 kAddr_ConsoleOpenHook = 0x71D665;
constexpr UInt32 kAddr_ConsoleCloseHook = 0x71D720;

static void DispatchConsoleOpenEvent()
{

    const std::vector<Script*> callbackSnapshot = OnConsoleHandler::g_openCallbacks;
    for (Script* callback : callbackSnapshot) {
        if (g_ochScript && callback) {
            g_ochScript->CallFunctionAlt(callback, nullptr, 0);
        }
    }
}

static void DispatchConsoleCloseEvent()
{

    const std::vector<Script*> callbackSnapshot = OnConsoleHandler::g_closeCallbacks;
    for (Script* callback : callbackSnapshot) {
        if (g_ochScript && callback) {
            g_ochScript->CallFunctionAlt(callback, nullptr, 0);
        }
    }
}

static void __cdecl Hook_ConsoleOpen()
{
    DispatchConsoleOpenEvent();
    if (OnConsoleHandler::s_originalOpenTarget) {
        ((void(*)())OnConsoleHandler::s_originalOpenTarget)();
    }
}

static void __cdecl Hook_ConsoleClose()
{
    DispatchConsoleCloseEvent();
    if (OnConsoleHandler::s_originalCloseTarget) {
        ((void(*)())OnConsoleHandler::s_originalCloseTarget)();
    }
}

static bool ReadCallTargetIfCall(UInt32 callAddr, UInt32& outTarget)
{
    if (*(UInt8*)callAddr != 0xE8)
    {
        return false;
    }

    outTarget = *(UInt32*)(callAddr + 1) + callAddr + 5;
    return true;
}

static void InitHooks()
{
    if (OnConsoleHandler::g_hooksInstalled) return;

    if (!ReadCallTargetIfCall(kAddr_ConsoleOpenHook, OnConsoleHandler::s_originalOpenTarget) ||
        !ReadCallTargetIfCall(kAddr_ConsoleCloseHook, OnConsoleHandler::s_originalCloseTarget))
    {
        return;
    }

    SafeWrite::WriteRelCall(kAddr_ConsoleOpenHook, (UInt32)Hook_ConsoleOpen);
    SafeWrite::WriteRelCall(kAddr_ConsoleCloseHook, (UInt32)Hook_ConsoleClose);

    OnConsoleHandler::g_hooksInstalled = true;
}

static bool AddOpenCallback(Script* callback)
{
    if (!callback) return false;

    for (Script* s : OnConsoleHandler::g_openCallbacks) {
        if (s == callback) {
            return false;
        }
    }

    OnConsoleHandler::g_openCallbacks.push_back(callback);

    if (!OnConsoleHandler::g_hooksInstalled)
        InitHooks();

    return true;
}

static bool RemoveOpenCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnConsoleHandler::g_openCallbacks.begin(); it != OnConsoleHandler::g_openCallbacks.end(); ++it) {
        if (*it == callback) {
            OnConsoleHandler::g_openCallbacks.erase(it);
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
            return false;
        }
    }

    OnConsoleHandler::g_closeCallbacks.push_back(callback);

    if (!OnConsoleHandler::g_hooksInstalled)
        InitHooks();

    return true;
}

static bool RemoveCloseCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnConsoleHandler::g_closeCallbacks.begin(); it != OnConsoleHandler::g_closeCallbacks.end(); ++it) {
        if (*it == callback) {
            OnConsoleHandler::g_closeCallbacks.erase(it);
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
        if (AddOpenCallback(callback)) {
            *result = 1;
        }
    } else {
        if (RemoveOpenCallback(callback)) {
            *result = 1;
        }
    }

    return true;
}

bool Cmd_SetOnConsoleCloseEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;

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
        if (AddCloseCallback(callback)) {
            *result = 1;
        }
    } else {
        if (RemoveCloseCallback(callback)) {
            *result = 1;
        }
    }

    return true;
}

bool OCH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_ochPluginHandle = nvse->GetPluginHandle();

    g_ochScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_ochScript) {
        return false;
    }

    g_ExtractArgsEx = g_ochScript->ExtractArgsEx;

    nvse->SetOpcodeBase(0x4003);
    nvse->RegisterCommand(&kCommandInfo_SetOnConsoleOpenEventHandler);
    g_ochOpenOpcode = 0x4003;

    nvse->SetOpcodeBase(0x4004);
    nvse->RegisterCommand(&kCommandInfo_SetOnConsoleCloseEventHandler);
    g_ochCloseOpcode = 0x4004;

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

void OCH_ClearCallbacks()
{
    OnConsoleHandler::g_openCallbacks.clear();
    OnConsoleHandler::g_closeCallbacks.clear();
}
