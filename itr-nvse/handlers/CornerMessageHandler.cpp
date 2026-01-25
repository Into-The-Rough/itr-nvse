//corner message event handler - fires when HUD notification displays
//hooks earlier than SetShowOffOnCornerMessageEventHandler to catch ALL messages including first on load

#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <Windows.h>

#include "CornerMessageHandler.h"


class TESForm;
class TESObjectREFR;
class Script;
class ScriptEventList;

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

static FILE* g_cmhLogFile = nullptr;

static void CMH_Log(const char* fmt, ...) {
    if (!g_cmhLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_cmhLogFile, fmt, args);
    fprintf(g_cmhLogFile, "\n");
    fflush(g_cmhLogFile);
    va_end(args);
}

static NVSEScriptInterface* g_cmhScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_registeredOpcode = 0;

enum eEmotion : UInt32 {
    kEmotion_Neutral = 0,
    kEmotion_Happy = 1,
    kEmotion_Sad = 2,
    kEmotion_Pain = 3,
};

struct CornerMessageCallback {
    Script* script;
};

static std::vector<CornerMessageCallback> g_callbacks;

struct QueuedMessage {
    std::string text;
    UInt32 emotion;
    std::string iconPath;
    std::string soundPath;
    float displayTime;
    bool instant;
};

static std::vector<QueuedMessage> g_queuedMessages;
static bool g_hasHandlers = false;

//HUDMainMenu::ShowNotify at 0x775380
//bool __thiscall HUDMainMenu::ShowNotify(const char* text, eEmotion emotion,
//    const char* iconPath, const char* soundName, float time, bool instant)
constexpr UInt32 kAddr_HUDMainMenu_ShowNotify = 0x775380;
constexpr int kPrologueBytes = 5; //push ebp; mov ebp, esp; push -1
static UInt32 g_originalShowNotify = 0;

static void DispatchCornerMessage(const char* text, UInt32 emotion,
    const char* iconPath, const char* soundPath, float displayTime)
{
    if (!g_cmhScript) return;

    const char* safeText = text ? text : "";
    const char* safeIcon = iconPath ? iconPath : "";
    const char* safeSound = soundPath ? soundPath : "";

    CMH_Log("Corner message: text='%s' emotion=%u icon='%s' sound='%s' time=%.2f",
            safeText, emotion, safeIcon, safeSound, displayTime);

    for (const auto& cb : g_callbacks) {
        if (cb.script) {
            CMH_Log("  Dispatching to callback 0x%08X", cb.script);
            g_cmhScript->CallFunctionAlt(cb.script, nullptr, 5,
                safeText, emotion, safeIcon, safeSound, *(UInt32*)&displayTime);
        }
    }
}

static bool __fastcall Hook_HUDMainMenu_ShowNotify(
    void* thisPtr,
    void* edx,
    const char* text,
    UInt32 emotion,
    const char* iconPath,
    const char* soundPath,
    float displayTime,
    bool instant
) {
    if (text && text[0]) {
        if (g_hasHandlers) {
            DispatchCornerMessage(text, emotion, iconPath, soundPath, displayTime);
        } else {
            QueuedMessage msg;
            msg.text = text;
            msg.emotion = emotion;
            msg.iconPath = iconPath ? iconPath : "";
            msg.soundPath = soundPath ? soundPath : "";
            msg.displayTime = displayTime;
            msg.instant = instant;
            g_queuedMessages.push_back(msg);
            CMH_Log("Queued early message: '%s' (queue size: %zu)", text, g_queuedMessages.size());
        }
    }

    return ((bool(__thiscall*)(void*, const char*, UInt32, const char*, const char*, float, bool))g_originalShowNotify)(
        thisPtr, text, emotion, iconPath, soundPath, displayTime, instant
    );
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
    PatchWrite8(src, 0xE9);
    PatchWrite32(src + 1, dst - src - 5);
}

static UInt8* g_trampolineAddr = nullptr;

static void InstallHook() {
    g_trampolineAddr = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampolineAddr) {
        CMH_Log("ERROR: Failed to allocate trampoline memory");
        return;
    }

    DWORD oldProtect;
    VirtualProtect((void*)kAddr_HUDMainMenu_ShowNotify, kPrologueBytes, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(g_trampolineAddr, (void*)kAddr_HUDMainMenu_ShowNotify, kPrologueBytes);
    VirtualProtect((void*)kAddr_HUDMainMenu_ShowNotify, kPrologueBytes, oldProtect, &oldProtect);

    g_trampolineAddr[kPrologueBytes] = 0xE9;
    *(UInt32*)(g_trampolineAddr + kPrologueBytes + 1) = (kAddr_HUDMainMenu_ShowNotify + kPrologueBytes) - ((UInt32)g_trampolineAddr + kPrologueBytes + 5);

    g_originalShowNotify = (UInt32)g_trampolineAddr;

    WriteRelJump(kAddr_HUDMainMenu_ShowNotify, (UInt32)Hook_HUDMainMenu_ShowNotify);

    CMH_Log("Hook installed at 0x%08X, trampoline at 0x%08X", kAddr_HUDMainMenu_ShowNotify, g_trampolineAddr);
}

static ParamInfo kParams_SetCornerMessageHandler[2] = {
    {"setOrRemove", kParamType_Integer, 0},
    {"handler",     kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(SetCornerMessageHandler,
    "Registers/unregisters callback for corner message events.",
    0, 2, kParams_SetCornerMessageHandler);

bool Cmd_SetCornerMessageHandler_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 setOrRemove = 0;
    TESForm* handlerForm = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &setOrRemove, &handlerForm)) {
        CMH_Log("SetCornerMessageHandler: Failed to extract args");
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
        CMH_Log("SetCornerMessageHandler: Invalid handler script");
        return true;
    }

    Script* script = (Script*)handlerForm;

    if (setOrRemove) {
        bool found = false;
        for (const auto& cb : g_callbacks) {
            if (cb.script == script) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_callbacks.push_back({script});
            g_hasHandlers = true;
            CMH_Log("SetCornerMessageHandler: Added callback 0x%08X", script);

            if (!g_queuedMessages.empty()) {
                CMH_Log("Replaying %zu queued messages to new handler", g_queuedMessages.size());
                for (const auto& msg : g_queuedMessages) {
                    if (g_cmhScript) {
                        g_cmhScript->CallFunctionAlt(script, nullptr, 5,
                            msg.text.c_str(), msg.emotion, msg.iconPath.c_str(),
                            msg.soundPath.c_str(), *(UInt32*)&msg.displayTime);
                    }
                }
                g_queuedMessages.clear();
            }
        }
        *result = 1;
    } else {
        for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
            if (it->script == script) {
                g_callbacks.erase(it);
                CMH_Log("SetCornerMessageHandler: Removed callback 0x%08X", script);
                *result = 1;
                break;
            }
        }
        g_hasHandlers = !g_callbacks.empty();
    }

    return true;
}

unsigned int CMH_GetOpcode() {
    return g_registeredOpcode;
}

bool CMH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\CornerMessageHandler.log");
    g_cmhLogFile = fopen(logPath, "w");

    CMH_Log("CornerMessageHandler initializing...");

    g_cmhScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_cmhScript) {
        CMH_Log("ERROR: Failed to get script interface");
        return false;
    }
    g_ExtractArgsEx = g_cmhScript->ExtractArgsEx;

    InstallHook();

    nvse->SetOpcodeBase(0x3B14);
    nvse->RegisterCommand(&kCommandInfo_SetCornerMessageHandler);
    g_registeredOpcode = 0x3B14;

    CMH_Log("Registered SetCornerMessageHandler at opcode 0x3B14");
    CMH_Log("CornerMessageHandler initialized successfully");
    return true;
}
