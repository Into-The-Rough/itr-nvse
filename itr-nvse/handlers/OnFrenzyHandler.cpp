//fires when an actor becomes frenzied (brain condition goes to 0)

#include <vector>
#include <Windows.h>

#include "OnFrenzyHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"

static NVSEScriptInterface* g_ofhScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_registeredOpcode = 0;

enum ActorValueCode {
    kAVCode_BrainCondition = 31,
    kAVCode_Aggression = 10,
};

struct FrenzyCallback {
    Script* script;
    TESForm* filter;  // nullptr = all actors, otherwise filter by actor/base
};

static std::vector<FrenzyCallback> g_callbacks;

//AV::LimbCondition::HandleChange - called when limb condition changes
//void __cdecl HandleChange(ActorValueOwner* a1, int avIndex, float oldValue, float delta, ActorValueOwner* attacker)
//prologue: push ebp; mov ebp,esp; sub esp,0x310 = 9 bytes
constexpr UInt32 kAddr_LimbCondition_HandleChange = 0x8B9240;
static Detours::JumpDetour s_detour;

static void DispatchFrenzyEvent(Actor* actor) {
    if (!actor) return;

    TESForm* baseForm = *(TESForm**)((UInt8*)actor + 0x20); //baseForm

    if (g_ofhScript) {
        const auto snapshot = g_callbacks;
        for (const auto& cb : snapshot) {
            bool shouldDispatch = !cb.filter
                || cb.filter == (TESForm*)actor
                || cb.filter == baseForm;

            if (shouldDispatch && cb.script)
                g_ofhScript->CallFunctionAlt(cb.script, nullptr, 1, (TESForm*)actor);
        }
    }

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnFrenzy", nullptr, (TESForm*)actor);
}

//void __cdecl AV::LimbCondition::HandleChange(
//    ActorValueOwner* a1,  //Actor+0xA4
//    int avIndex, float oldValue, float delta, ActorValueOwner* attacker)

static void __cdecl Hook_LimbCondition_HandleChange(
    void* actorValueOwner,  //Actor+0xA4
    UInt32 avIndex,
    float oldValue,
    float delta,
    void* attackerAVO
) {
    Actor* actor = actorValueOwner ? (Actor*)((UInt8*)actorValueOwner - 0xA4) : nullptr; //AVO at Actor+0xA4

    bool willFrenzy = false;
    if (avIndex == kAVCode_BrainCondition && oldValue > 0.0f && delta < 0.0f) {
        float newValue = oldValue + delta;
        if (newValue <= 0.0f) {
            willFrenzy = true;
        }
    }

    typedef void(__cdecl* HandleChange_t)(void*, UInt32, float, float, void*);
    s_detour.GetTrampoline<HandleChange_t>()(actorValueOwner, avIndex, oldValue, delta, attackerAVO);

    if (willFrenzy && actor) {
        DispatchFrenzyEvent(actor);
    }
}

static void InstallHook() {
    if (!s_detour.WriteRelJump(kAddr_LimbCondition_HandleChange, Hook_LimbCondition_HandleChange, 9)) {
        return;
    }
}

static ParamInfo kParams_SetOnFrenzyEventHandler[3] = {
    {"setOrRemove", kParamType_Integer, 0},
    {"handler",     kParamType_AnyForm, 0},
    {"filter",      kParamType_AnyForm, 1},
};

DEFINE_COMMAND_PLUGIN(SetOnFrenzyEventHandler,
    "Registers/unregisters callback for frenzy events (brain condition reaches 0).",
    0, 3, kParams_SetOnFrenzyEventHandler);

bool Cmd_SetOnFrenzyEventHandler_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 setOrRemove = 0;
    TESForm* handlerForm = nullptr;
    TESForm* filter = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &setOrRemove, &handlerForm, &filter)) {
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
        return true;
    }

    Script* script = (Script*)handlerForm;

    if (setOrRemove) {
        bool found = false;
        for (const auto& cb : g_callbacks) {
            if (cb.script == script && cb.filter == filter) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_callbacks.push_back({script, filter});
        }
        *result = 1;
    } else {
        for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
            if (it->script == script && it->filter == filter) {
                g_callbacks.erase(it);
                *result = 1;
                break;
            }
        }
    }

    return true;
}

unsigned int OFH_GetOpcode() {
    return g_registeredOpcode;
}

bool OFH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    g_ofhScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_ofhScript) {
        return false;
    }
    g_ExtractArgsEx = g_ofhScript->ExtractArgsEx;

    InstallHook();

    nvse->SetOpcodeBase(0x4012);
    nvse->RegisterCommand(&kCommandInfo_SetOnFrenzyEventHandler);
    g_registeredOpcode = 0x4012;

    return true;
}

void OFH_ClearCallbacks()
{
    g_callbacks.clear();
}
