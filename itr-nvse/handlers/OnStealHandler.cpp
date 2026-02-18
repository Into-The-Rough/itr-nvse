//hooks Actor::StealAlarm (0x8BFA40) to dispatch events when items are stolen

#include <vector>
#include <cstring>
#include <Windows.h>

#include "OnStealHandler.h"
#include "internal/NVSEMinimal.h"

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

    static Actor* s_thief = nullptr;
    static TESObjectREFR* s_target = nullptr;
    static TESForm* s_item = nullptr;
    static SInt32 s_quantity = 0;
    static SInt32 s_value = 0;
    static TESObjectREFR* s_owner = nullptr;
}

//Actor::StealAlarm - prologue: push ebp; mov ebp,esp; push -1 (5 bytes)
constexpr UInt32 kAddr_StealAlarm = 0x8BFA40;
constexpr UInt32 kAddr_StealAlarmBody = 0x8BFA45;

static void DispatchStealEvent()
{

    if (OnStealHandler::g_callbacks.empty()) return;

    //snapshot for reentrancy safety
    std::vector<Script*> snapshot;
    snapshot.reserve(OnStealHandler::g_callbacks.size());
    for (const auto& entry : OnStealHandler::g_callbacks)
        if (entry.callback) snapshot.push_back(entry.callback);

    for (Script* cb : snapshot) {
        if (g_oshScript) {
            g_oshScript->CallFunctionAlt(
                cb,
                reinterpret_cast<TESObjectREFR*>(OnStealHandler::s_thief),
                5,
                OnStealHandler::s_thief,
                OnStealHandler::s_target,
                OnStealHandler::s_item,
                OnStealHandler::s_owner,
                OnStealHandler::s_quantity
            );
        }
    }
}

static __declspec(naked) void StealAlarmHook()
{
    __asm
    {
        mov     OnStealHandler::s_thief, ecx

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

        pushad
        pushfd
        call    DispatchStealEvent
        popfd
        popad

        //original prologue
        push    ebp
        mov     ebp, esp
        push    0FFFFFFFFh

        mov     eax, kAddr_StealAlarmBody
        jmp     eax
    }
}

static void InitHook()
{
    if (OnStealHandler::g_hookInstalled) return;

    SafeWrite::WriteRelJump(kAddr_StealAlarm, (UInt32)StealAlarmHook);

    OnStealHandler::g_hookInstalled = true;
}

static bool AddCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (const auto& entry : OnStealHandler::g_callbacks) {
        if (entry.callback == callback) {
            return false;
        }
    }

    OnStealHandler::g_callbacks.emplace_back(callback);

    if (!OnStealHandler::g_hookInstalled) {
        InitHook();
    }

    return true;
}

static bool RemoveCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnStealHandler::g_callbacks.begin(); it != OnStealHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback) {
            OnStealHandler::g_callbacks.erase(it);
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
        if (AddCallback_Internal(callback)) {
            *result = 1;
        }
    } else {
        if (RemoveCallback_Internal(callback)) {
            *result = 1;
        }
    }

    return true;
}

bool OSH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_oshPluginHandle = nvse->GetPluginHandle();

    g_oshScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_oshScript) {
        return false;
    }

    g_ExtractArgsEx = g_oshScript->ExtractArgsEx;

    NVSEDataInterface* nvseData = reinterpret_cast<NVSEDataInterface*>(
        nvse->QueryInterface(kInterface_Data));

    if (nvseData) {
        g_CaptureLambdaVars = reinterpret_cast<_CaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList));
        g_UncaptureLambdaVars = reinterpret_cast<_UncaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList));
    }

    nvse->SetOpcodeBase(0x4001);
    nvse->RegisterCommand(&kCommandInfo_SetOnStealEventHandler);
    g_oshOpcode = 0x4001;

    return true;
}

unsigned int OSH_GetOpcode()
{
    return g_oshOpcode;
}

void OSH_ClearCallbacks()
{
    OnStealHandler::g_callbacks.clear();
}
