//hooks Actor::SetAnimAction call at 0x894081 in FiresWeapon when weapon jams

#include <vector>
#include <Windows.h>

#include "OnWeaponJamHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

static PluginHandle g_owjhPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_owjhScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_owjhOpcode = 0;

//static variables for hook - must be outside namespace for inline asm
static Actor* g_jamActor = nullptr;

namespace OnWeaponJamHandler {
    std::vector<Script*> g_callbacks;
    bool g_hookInstalled = false;

    static UInt32 s_originalSetAnimAction = 0x8A73E0; //Actor::SetAnimAction
}

//call to SetAnimAction(action=9 jam) in FiresWeapon
constexpr UInt32 kAddr_JamSetAnimActionCall = 0x894081;

constexpr UInt32 kOffset_MobileObject_pCurrentProcess = 0x68;
constexpr UInt32 kVtableIndex_GetCurrentWeapon = 82;
constexpr UInt32 kOffset_ItemChange_pObject = 0x08;

static TESObjectWEAP* GetActorCurrentWeapon(Actor* actor)
{
    if (!actor) {
        return nullptr;
    }

    UInt32 pProcess = *(UInt32*)((UInt8*)actor + kOffset_MobileObject_pCurrentProcess);
    if (!pProcess) {
        return nullptr;
    }

    UInt32 vtable = *(UInt32*)pProcess;
    if (!vtable) {
        return nullptr;
    }

    UInt32 funcAddr = *(UInt32*)(vtable + kVtableIndex_GetCurrentWeapon * 4);

    typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32 pProcess);
    GetCurrentWeapon_t GetCurrentWeapon = (GetCurrentWeapon_t)funcAddr;

    UInt32 itemChange = GetCurrentWeapon(pProcess);
    if (!itemChange) {
        return nullptr;
    }

    TESObjectWEAP* weapon = (TESObjectWEAP*)(*(UInt32*)(itemChange + kOffset_ItemChange_pObject));
    return weapon;
}

static void DispatchWeaponJamEvent()
{

    if (!g_jamActor) {
        return;
    }

    TESObjectWEAP* weapon = GetActorCurrentWeapon(g_jamActor);

    for (Script* callback : OnWeaponJamHandler::g_callbacks) {
        if (g_owjhScript && callback) {
            g_owjhScript->CallFunctionAlt(
                callback,
                reinterpret_cast<TESObjectREFR*>(g_jamActor),
                2,
                g_jamActor,
                weapon
            );
        }
    }

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnWeaponJam",
            reinterpret_cast<TESObjectREFR*>(g_jamActor),
            g_jamActor, weapon);
}

static UInt32 s_SetAnimActionAddr = 0x8A73E0;

static __declspec(naked) void Hook_SetAnimAction_Jam()
{
    __asm {
        mov g_jamActor, ecx
        pushad
        pushfd
        call DispatchWeaponJamEvent
        popfd
        popad

        jmp dword ptr [s_SetAnimActionAddr]
    }
}

static void InitHook()
{
    if (OnWeaponJamHandler::g_hookInstalled) {
        return;
    }

    SafeWrite::WriteRelCall(kAddr_JamSetAnimActionCall, (UInt32)Hook_SetAnimAction_Jam);

    OnWeaponJamHandler::g_hookInstalled = true;
}

static bool AddCallback(Script* callback)
{
    if (!callback) {
        return false;
    }

    for (Script* s : OnWeaponJamHandler::g_callbacks) {
        if (s == callback) {
            return false;
        }
    }

    OnWeaponJamHandler::g_callbacks.push_back(callback);

    if (!OnWeaponJamHandler::g_hookInstalled) {
        InitHook();
    }

    return true;
}

static bool RemoveCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnWeaponJamHandler::g_callbacks.begin(); it != OnWeaponJamHandler::g_callbacks.end(); ++it) {
        if (*it == callback) {
            OnWeaponJamHandler::g_callbacks.erase(it);
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
        if (AddCallback(callback)) {
            *result = 1;
        }
    } else {
        if (RemoveCallback(callback)) {
            *result = 1;
        }
    }

    return true;
}

bool OWJH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_owjhPluginHandle = nvse->GetPluginHandle();

    g_owjhScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_owjhScript) {
        return false;
    }

    g_ExtractArgsEx = g_owjhScript->ExtractArgsEx;

    nvse->SetOpcodeBase(0x4005);
    nvse->RegisterCommand(&kCommandInfo_SetOnWeaponJamEventHandler);
    g_owjhOpcode = 0x4005;

    InitHook();
    return true;
}

unsigned int OWJH_GetOpcode()
{
    return g_owjhOpcode;
}

void OWJH_ClearCallbacks()
{
    OnWeaponJamHandler::g_callbacks.clear();
}
