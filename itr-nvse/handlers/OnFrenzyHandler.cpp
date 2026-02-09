//fires when an actor becomes frenzied (brain condition goes to 0)

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnFrenzyHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

static FILE* g_ofhLogFile = nullptr;

static void OFH_Log(const char* fmt, ...) {
    if (!g_ofhLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_ofhLogFile, fmt, args);
    fprintf(g_ofhLogFile, "\n");
    fflush(g_ofhLogFile);
    va_end(args);
}

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
// void __cdecl HandleChange(ActorValueOwner* a1, int avIndex, float oldValue, float delta, ActorValueOwner* attacker)
// Prologue: push ebp; mov ebp, esp; sub esp, 0x310 = 9 bytes
constexpr UInt32 kAddr_LimbCondition_HandleChange = 0x8B9240;
static Detours::JumpDetour s_detour;

static void DispatchFrenzyEvent(Actor* actor) {
    if (!g_ofhScript || !actor) return;

    OFH_Log("Frenzy event: actor=0x%08X", actor);

    // Get actor's base form for filter comparison
    TESForm* baseForm = *(TESForm**)((UInt8*)actor + 0x20);  // baseForm at offset 0x20

    for (const auto& cb : g_callbacks) {
        bool shouldDispatch = false;

        if (!cb.filter) {
            // No filter - dispatch for all actors
            shouldDispatch = true;
        } else {
            // Check if filter matches actor ref or base form
            if (cb.filter == (TESForm*)actor || cb.filter == baseForm) {
                shouldDispatch = true;
            }
        }

        if (shouldDispatch && cb.script) {
            OFH_Log("  Dispatching to callback 0x%08X", cb.script);
            g_ofhScript->CallFunctionAlt(cb.script, nullptr, 1, (TESForm*)actor);
        }
    }
}

//original function type
// void __cdecl AV::LimbCondition::HandleChange(
//     ActorValueOwner* a1,  // Actor + 0xA4
//     int avIndex,
//     float oldValue,
//     float delta,
//     ActorValueOwner* attacker
// )

static void __cdecl Hook_LimbCondition_HandleChange(
    void* actorValueOwner,  // ActorValueOwner is at Actor + 0xA4
    UInt32 avIndex,
    float oldValue,
    float delta,
    void* attackerAVO
) {
    // Get Actor pointer from ActorValueOwner (AVO is at offset 0xA4 in Actor)
    Actor* actor = actorValueOwner ? (Actor*)((UInt8*)actorValueOwner - 0xA4) : nullptr;

    // Check if this is brain condition going from positive to <= 0
    bool willFrenzy = false;
    if (avIndex == kAVCode_BrainCondition && oldValue > 0.0f && delta < 0.0f) {
        // New value will be oldValue + delta (delta is negative for damage)
        // Actually, looking at the code, oldValue is the value BEFORE the change
        // and the function reads the current value (after change) to check
        // So we check: old > 0 AND current <= 0
        // But we're called before the original, so let's check if it will go to 0
        float newValue = oldValue + delta;
        if (newValue <= 0.0f) {
            willFrenzy = true;
            OFH_Log("Brain condition going to 0: actor=0x%08X old=%.1f delta=%.1f new=%.1f",
                    actor, oldValue, delta, newValue);
        }
    }

    // Call original
    typedef void(__cdecl* HandleChange_t)(void*, UInt32, float, float, void*);
    s_detour.GetTrampoline<HandleChange_t>()(actorValueOwner, avIndex, oldValue, delta, attackerAVO);

    // Dispatch frenzy event if brain went to 0
    if (willFrenzy && actor && g_callbacks.size() > 0) {
        DispatchFrenzyEvent(actor);
    }
}

static void InstallHook() {
    if (!s_detour.WriteRelJump(kAddr_LimbCondition_HandleChange, Hook_LimbCondition_HandleChange, 9)) {
        OFH_Log("ERROR: Failed to install hook");
        return;
    }
    OFH_Log("Hook installed at 0x%08X", kAddr_LimbCondition_HandleChange);
}

static ParamInfo kParams_SetOnFrenzyEventHandler[3] = {
    {"setOrRemove", kParamType_Integer, 0},
    {"handler",     kParamType_AnyForm, 0},
    {"filter",      kParamType_AnyForm, 1},  // Optional
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
        OFH_Log("SetOnFrenzyEventHandler: Failed to extract args");
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
        OFH_Log("SetOnFrenzyEventHandler: Invalid handler script");
        return true;
    }

    Script* script = (Script*)handlerForm;

    if (setOrRemove) {
        // Add callback
        bool found = false;
        for (const auto& cb : g_callbacks) {
            if (cb.script == script && cb.filter == filter) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_callbacks.push_back({script, filter});
            OFH_Log("SetOnFrenzyEventHandler: Added callback 0x%08X filter=0x%08X", script, filter);
        }
        *result = 1;
    } else {
        // Remove callback
        for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
            if (it->script == script && it->filter == filter) {
                g_callbacks.erase(it);
                OFH_Log("SetOnFrenzyEventHandler: Removed callback 0x%08X", script);
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

    // Log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnFrenzyHandler.log");
    //g_ofhLogFile = fopen(logPath, "w"); //disabled for release

    OFH_Log("OnFrenzyHandler initializing...");

    // Get script interface
    g_ofhScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_ofhScript) {
        OFH_Log("ERROR: Failed to get script interface");
        return false;
    }
    g_ExtractArgsEx = g_ofhScript->ExtractArgsEx;

    // Install hook
    InstallHook();

    // Register command
    nvse->SetOpcodeBase(0x4012);
    nvse->RegisterCommand(&kCommandInfo_SetOnFrenzyEventHandler);
    g_registeredOpcode = 0x4012;

    OFH_Log("Registered SetOnFrenzyEventHandler at opcode 0x4012");
    OFH_Log("OnFrenzyHandler initialized successfully");
    return true;
}

void OFH_ClearCallbacks()
{
    g_callbacks.clear();
    OFH_Log("Callbacks cleared on game load");
}
