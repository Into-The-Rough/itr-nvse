//hooks Actor::TryDropWeapon (0x89F580) to dispatch events when actors drop weapons

#include <vector>
#include <Windows.h>

#include "OnWeaponDropHandler.h"
#include "internal/NVSEMinimal.h"

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

    static Actor* s_actor = nullptr;
    static TESObjectWEAP* s_weapon = nullptr;
}

//Actor::TryDropWeapon - prologue: push ebp; mov ebp,esp; sub esp,3Ch (6 bytes)
constexpr UInt32 kAddr_TryDropWeapon = 0x89F580;
constexpr UInt32 kAddr_TryDropWeaponBody = 0x89F586;

static TESObjectWEAP* GetActorCurrentWeapon(Actor* actor)
{
    if (!actor) return nullptr;

    UInt32 pProcess = *(UInt32*)((UInt8*)actor + 0x68); //MobileObject::pCurrentProcess
    if (!pProcess) return nullptr;

    UInt32 vtable = *(UInt32*)pProcess;
    if (!vtable) return nullptr;

    typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32 pProcess); //vtable[82]
    GetCurrentWeapon_t GetCurrentWeapon = (GetCurrentWeapon_t)(*(UInt32*)(vtable + 82 * 4));

    UInt32 itemChange = GetCurrentWeapon(pProcess);
    if (!itemChange) return nullptr;

    return (TESObjectWEAP*)(*(UInt32*)(itemChange + 0x08)); //ContChangesEntry::pObject
}

static void DispatchWeaponDropEvent()
{
    OnWeaponDropHandler::s_weapon = GetActorCurrentWeapon(OnWeaponDropHandler::s_actor);

    if (OnWeaponDropHandler::g_callbacks.empty()) return;
    if (!OnWeaponDropHandler::s_weapon) return;

    //snapshot for reentrancy safety
    std::vector<Script*> snapshot;
    snapshot.reserve(OnWeaponDropHandler::g_callbacks.size());
    for (const auto& entry : OnWeaponDropHandler::g_callbacks)
        if (entry.callback) snapshot.push_back(entry.callback);

    for (Script* cb : snapshot) {
        if (g_owdhScript) {
            g_owdhScript->CallFunctionAlt(
                cb,
                reinterpret_cast<TESObjectREFR*>(OnWeaponDropHandler::s_actor),
                2,
                OnWeaponDropHandler::s_actor,
                OnWeaponDropHandler::s_weapon
            );
        }
    }
}

static __declspec(naked) void TryDropWeaponHook()
{
    __asm
    {
        mov     OnWeaponDropHandler::s_actor, ecx

        pushad
        pushfd
        call    DispatchWeaponDropEvent
        popfd
        popad

        //original prologue
        push    ebp
        mov     ebp, esp
        sub     esp, 3Ch

        mov     eax, kAddr_TryDropWeaponBody
        jmp     eax
    }
}

static void InitHook()
{
    if (OnWeaponDropHandler::g_hookInstalled) return;

    SafeWrite::WriteRelJump(kAddr_TryDropWeapon, (UInt32)TryDropWeaponHook);
    SafeWrite::Write8(kAddr_TryDropWeapon + 5, 0x90); //nop 6th byte

    OnWeaponDropHandler::g_hookInstalled = true;
}

static bool AddCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (const auto& entry : OnWeaponDropHandler::g_callbacks) {
        if (entry.callback == callback) {
            return false;
        }
    }

    OnWeaponDropHandler::g_callbacks.emplace_back(callback);

    if (!OnWeaponDropHandler::g_hookInstalled) {
        InitHook();
    }

    return true;
}

static bool RemoveCallback_Internal(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnWeaponDropHandler::g_callbacks.begin(); it != OnWeaponDropHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback) {
            OnWeaponDropHandler::g_callbacks.erase(it);
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

bool OWDH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_owdhPluginHandle = nvse->GetPluginHandle();

    g_owdhScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_owdhScript) {
        return false;
    }

    g_ExtractArgsEx = g_owdhScript->ExtractArgsEx;

    nvse->SetOpcodeBase(0x4002);
    nvse->RegisterCommand(&kCommandInfo_SetOnWeaponDropEventHandler);
    g_owdhOpcode = 0x4002;

    return true;
}

unsigned int OWDH_GetOpcode()
{
    return g_owdhOpcode;
}

void OWDH_ClearCallbacks()
{
    OnWeaponDropHandler::g_callbacks.clear();
}
