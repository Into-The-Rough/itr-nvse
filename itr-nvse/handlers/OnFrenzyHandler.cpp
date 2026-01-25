//fires when an actor becomes frenzied (brain condition goes to 0)

#include <cstdint>
#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnFrenzyHandler.h"


class TESForm;
class TESObjectREFR;
class Script;
class ScriptEventList;
class Actor;

struct CommandInfo;
struct ParamInfo;

using PluginHandle = UInt32;

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

enum { kInterface_Script = 6 };
enum { kRetnType_Default = 0 };

struct NVSEArrayVarInterface {
    struct Element { UInt8 pad[16]; };
};

struct NVSEScriptInterface {
    bool (*CallFunction)(Script*, TESObjectREFR*, TESObjectREFR*, NVSEArrayVarInterface::Element*, UInt8, ...);
    int (*GetFunctionParams)(Script*, UInt8*);
    bool (*ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...);
    bool (*ExtractFormatStringArgs)(UInt32, char*, ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, UInt32, ...);
    bool (*CallFunctionAlt)(Script*, TESObjectREFR*, UInt8, ...);
};

#define COMMAND_ARGS void* paramInfo, void* scriptData, TESObjectREFR* thisObj, \
    UInt32 containingObj, Script* scriptObj, ScriptEventList* eventList, \
    double* result, UInt32* opcodeOffsetPtr

using CommandExecuteFunc = bool (*)(COMMAND_ARGS);

struct ParamInfo {
    const char* typeStr;
    UInt32 typeID;
    UInt32 isOptional;
};

struct CommandInfo {
    const char* longName;
    const char* shortName;
    UInt32 opcode;
    const char* helpText;
    UInt16 needsParent;
    UInt16 numParams;
    ParamInfo* params;
    CommandExecuteFunc execute;
    void* parse;
    void* eval;
    UInt32 flags;
};

enum { kParamType_Integer = 0x01, kParamType_AnyForm = 0x3D };
enum { kFormType_Script = 0x11 };

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

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
// Prologue: 55 8B EC 81 EC 10 03 00 00 = push ebp; mov ebp, esp; sub esp, 0x310 (9 bytes)
constexpr UInt32 kAddr_LimbCondition_HandleChange = 0x8B9240;
constexpr int kPrologueBytes = 9;
static UInt32 g_originalHandleChange = 0;

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
    ((void(__cdecl*)(void*, UInt32, float, float, void*))g_originalHandleChange)(
        actorValueOwner, avIndex, oldValue, delta, attackerAVO
    );

    // Dispatch frenzy event if brain went to 0
    if (willFrenzy && actor && g_callbacks.size() > 0) {
        DispatchFrenzyEvent(actor);
    }
}

static void PatchWrite32(UInt32 addr, UInt32 data) {
    DWORD oldProtect;
    VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(UInt32*)addr = data;
    VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
}

static void PatchWrite8(UInt32 addr, UInt8 data) {
    DWORD oldProtect;
    VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(UInt8*)addr = data;
    VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
}

static void WriteRelJump(UInt32 src, UInt32 dst) {
    PatchWrite8(src, 0xE9);  // jmp rel32
    PatchWrite32(src + 1, dst - src - 5);
}

// Trampoline for original function
static UInt8* g_trampolineAddr = nullptr;

static void InstallHook() {
    // Allocate executable memory for trampoline
    g_trampolineAddr = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampolineAddr) {
        OFH_Log("ERROR: Failed to allocate trampoline memory");
        return;
    }

    // Copy original prologue bytes (9 bytes: push ebp; mov ebp, esp; sub esp, 0x310)
    DWORD oldProtect;
    VirtualProtect((void*)kAddr_LimbCondition_HandleChange, kPrologueBytes, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(g_trampolineAddr, (void*)kAddr_LimbCondition_HandleChange, kPrologueBytes);
    VirtualProtect((void*)kAddr_LimbCondition_HandleChange, kPrologueBytes, oldProtect, &oldProtect);

    // Add jump back to original + kPrologueBytes
    g_trampolineAddr[kPrologueBytes] = 0xE9;  // jmp
    *(UInt32*)(g_trampolineAddr + kPrologueBytes + 1) = (kAddr_LimbCondition_HandleChange + kPrologueBytes) - ((UInt32)g_trampolineAddr + kPrologueBytes + 5);

    g_originalHandleChange = (UInt32)g_trampolineAddr;

    // Write jump to our hook at the original location (overwrites 5 bytes, NOP remaining 4)
    WriteRelJump(kAddr_LimbCondition_HandleChange, (UInt32)Hook_LimbCondition_HandleChange);
    for (int i = 5; i < kPrologueBytes; i++) {
        PatchWrite8(kAddr_LimbCondition_HandleChange + i, 0x90);  // NOP
    }

    OFH_Log("Hook installed at 0x%08X, trampoline at 0x%08X", kAddr_LimbCondition_HandleChange, g_trampolineAddr);
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
    g_ofhLogFile = fopen(logPath, "w");

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
    nvse->SetOpcodeBase(0x3B13);
    nvse->RegisterCommand(&kCommandInfo_SetOnFrenzyEventHandler);
    g_registeredOpcode = 0x3B13;

    OFH_Log("Registered SetOnFrenzyEventHandler at opcode 0x3B13");
    OFH_Log("OnFrenzyHandler initialized successfully");
    return true;
}
