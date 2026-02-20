//provides DisableKeyEx/EnableKeyEx wrapper commands that fire events

#include <vector>
#include <Windows.h>

#include "OnKeyStateHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include <cstdio>

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
    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnKeyDisabled", nullptr, (int)keycode, (int)mask);

    if (OnKeyStateHandler::g_disabledCallbacks.empty()) {
        return;
    }

    for (Script* callback : OnKeyStateHandler::g_disabledCallbacks) {
        if (g_okshScript && callback) {
            g_okshScript->CallFunctionAlt(callback, nullptr, 2, keycode, mask);
        }
    }
}

static void DispatchKeyEnabledEvent(UInt32 keycode, UInt32 mask)
{
    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnKeyEnabled", nullptr, (int)keycode, (int)mask);

    if (OnKeyStateHandler::g_enabledCallbacks.empty()) {
        return;
    }

    for (Script* callback : OnKeyStateHandler::g_enabledCallbacks) {
        if (g_okshScript && callback) {
            g_okshScript->CallFunctionAlt(callback, nullptr, 2, keycode, mask);
        }
    }
}

static bool AddDisabledCallback(Script* callback)
{
    if (!callback) return false;

    for (Script* s : OnKeyStateHandler::g_disabledCallbacks) {
        if (s == callback) {
            return false;
        }
    }

    OnKeyStateHandler::g_disabledCallbacks.push_back(callback);
    return true;
}

static bool RemoveDisabledCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnKeyStateHandler::g_disabledCallbacks.begin();
         it != OnKeyStateHandler::g_disabledCallbacks.end(); ++it) {
        if (*it == callback) {
            OnKeyStateHandler::g_disabledCallbacks.erase(it);
            return true;
        }
    }
    return false;
}

static bool AddEnabledCallback(Script* callback)
{
    if (!callback) return false;

    for (Script* s : OnKeyStateHandler::g_enabledCallbacks) {
        if (s == callback) {
            return false;
        }
    }

    OnKeyStateHandler::g_enabledCallbacks.push_back(callback);
    return true;
}

static bool RemoveEnabledCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnKeyStateHandler::g_enabledCallbacks.begin();
         it != OnKeyStateHandler::g_enabledCallbacks.end(); ++it) {
        if (*it == callback) {
            OnKeyStateHandler::g_enabledCallbacks.erase(it);
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
        if (AddDisabledCallback(callback)) {
            *result = 1;
        }
    } else {
        if (RemoveDisabledCallback(callback)) {
            *result = 1;
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
        if (AddEnabledCallback(callback)) {
            *result = 1;
        }
    } else {
        if (RemoveEnabledCallback(callback)) {
            *result = 1;
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
        return true;
    }

    char cmd[64];
    if (mask)
        sprintf_s(cmd, "DisableKey %d %d", keycode, mask);
    else
        sprintf_s(cmd, "DisableKey %d", keycode);

    if (g_okshConsole) {
        g_okshConsole->RunScriptLine(cmd, nullptr);
    }

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
        return true;
    }

    char cmd[64];
    if (mask)
        sprintf_s(cmd, "EnableKey %d %d", keycode, mask);
    else
        sprintf_s(cmd, "EnableKey %d", keycode);

    if (g_okshConsole) {
        g_okshConsole->RunScriptLine(cmd, nullptr);
    }

    DispatchKeyEnabledEvent(keycode, mask);

    *result = 1;
    return true;
}

bool OKSH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_okshPluginHandle = nvse->GetPluginHandle();

    g_okshScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_okshScript) {
        return false;
    }

    g_ExtractArgsEx = g_okshScript->ExtractArgsEx;

    g_okshConsole = reinterpret_cast<NVSEConsoleInterface*>(
        nvse->QueryInterface(kInterface_Console));

    if (!g_okshConsole) {
        return false;
    }

    nvse->SetOpcodeBase(0x4006);
    nvse->RegisterCommand(&kCommandInfo_SetOnKeyDisabledEventHandler);
    g_okshDisabledOpcode = 0x4006;

    nvse->SetOpcodeBase(0x4007);
    nvse->RegisterCommand(&kCommandInfo_SetOnKeyEnabledEventHandler);
    g_okshEnabledOpcode = 0x4007;

    nvse->SetOpcodeBase(0x4008);
    nvse->RegisterCommand(&kCommandInfo_DisableKeyEx);

    nvse->SetOpcodeBase(0x4009);
    nvse->RegisterCommand(&kCommandInfo_EnableKeyEx);

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

void OKSH_ClearCallbacks()
{
    OnKeyStateHandler::g_disabledCallbacks.clear();
    OnKeyStateHandler::g_enabledCallbacks.clear();
}
