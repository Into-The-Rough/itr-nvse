//double tap event system - fires when key pressed twice within threshold

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <Windows.h>

#include "DoubleTapHandler.h"


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

enum { kParamType_Integer = 0x01, kParamType_Float = 0x02, kParamType_AnyForm = 0x3D };
enum { kFormType_Script = 0x11 };

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

static FILE* g_dthLogFile = nullptr;

static void DTH_Log(const char* fmt, ...) {
    if (!g_dthLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_dthLogFile, fmt, args);
    fprintf(g_dthLogFile, "\n");
    fflush(g_dthLogFile);
    va_end(args);
}

static NVSEScriptInterface* g_dthScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_nextHandlerId = 1;
static DWORD g_lastTickCount = 0;
static float g_currentTime = 0.0f;

struct DoubleTapHandler {
    UInt32 id;
    UInt32 key;
    float maxInterval;
    bool useControlCode;
    Script* callback;
};

struct TapState {
    bool isDown;
    bool wasReleased;
    float lastPressTime;
};

static std::vector<DoubleTapHandler> g_handlers;
static std::unordered_map<UInt32, TapState> g_tapStates;

static int MapDIKToVK(UInt32 dik) {
    static const int dikToVk[256] = {
        0, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5,
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_RSHIFT, VK_MULTIPLY,
        VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10,
        VK_NUMLOCK, VK_SCROLL, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_SUBTRACT, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6,
        VK_ADD, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL, 0, 0, VK_OEM_102, VK_F11, VK_F12
    };
    return (dik < 256) ? dikToVk[dik] : 0;
}

static bool IsRawKeyPressed(UInt32 keycode) {
    int vk = MapDIKToVK(keycode);
    if (vk == 0) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void* g_OSInputGlobals = nullptr;
typedef bool (__thiscall *GetControlState_t)(void*, UInt32, UInt32);
static GetControlState_t GetControlState = nullptr;

static bool IsControlPressed(UInt32 controlCode) {
    if (!g_OSInputGlobals || !GetControlState) return false;
    return GetControlState(g_OSInputGlobals, controlCode, 0);
}

static void DispatchDoubleTapEvent(Script* callback, UInt32 key) {
    if (!g_dthScript || !callback) return;
    DTH_Log("  Dispatching: key=%d", key);
    g_dthScript->CallFunctionAlt(callback, nullptr, 1, key);
}

void DTH_Update() {
    DWORD currentTick = GetTickCount();
    if (g_lastTickCount == 0) g_lastTickCount = currentTick;
    float deltaTime = (currentTick - g_lastTickCount) / 1000.0f;
    g_lastTickCount = currentTick;
    g_currentTime += deltaTime;

    if (!g_OSInputGlobals) {
        g_OSInputGlobals = *(void**)0x11F35CC;
        GetControlState = (GetControlState_t)0xA24660;
    }

    for (const auto& handler : g_handlers) {
        bool keyDown = handler.useControlCode
            ? IsControlPressed(handler.key)
            : IsRawKeyPressed(handler.key);

        TapState& state = g_tapStates[handler.key];

        if (keyDown && !state.isDown) {
            float timeSinceLastPress = g_currentTime - state.lastPressTime;

            if (state.wasReleased && timeSinceLastPress <= handler.maxInterval) {
                DTH_Log("Double tap detected: key=%d interval=%.3f", handler.key, timeSinceLastPress);
                DispatchDoubleTapEvent(handler.callback, handler.key);
                state.wasReleased = false;
            }

            state.isDown = true;
            state.lastPressTime = g_currentTime;
        }
        else if (!keyDown && state.isDown) {
            state.isDown = false;
            state.wasReleased = true;
        }
    }
}

static ParamInfo kParams_RegisterDoubleTap[3] = {
    {"keycode",     kParamType_Integer, 0},
    {"maxInterval", kParamType_Float,   0},
    {"callback",    kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(RegisterKeyDoubleTap,
    "Registers callback for key double-tap events. Returns handler ID.",
    0, 3, kParams_RegisterDoubleTap);

bool Cmd_RegisterKeyDoubleTap_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 keycode = 0;
    float maxInterval = 0;
    TESForm* callbackForm = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &keycode, &maxInterval, &callbackForm)) {
        DTH_Log("RegisterKeyDoubleTap: Failed to extract args");
        return true;
    }

    if (!callbackForm || *((UInt8*)callbackForm + 4) != kFormType_Script) {
        DTH_Log("RegisterKeyDoubleTap: Invalid callback");
        return true;
    }

    DoubleTapHandler handler;
    handler.id = g_nextHandlerId++;
    handler.key = keycode;
    handler.maxInterval = maxInterval;
    handler.useControlCode = false;
    handler.callback = (Script*)callbackForm;

    g_handlers.push_back(handler);
    *result = handler.id;

    DTH_Log("RegisterKeyDoubleTap: id=%d key=%d maxInterval=%.2f",
            handler.id, keycode, maxInterval);
    return true;
}

DEFINE_COMMAND_PLUGIN(RegisterControlDoubleTap,
    "Registers callback for control double-tap events. Returns handler ID.",
    0, 3, kParams_RegisterDoubleTap);

bool Cmd_RegisterControlDoubleTap_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 controlCode = 0;
    float maxInterval = 0;
    TESForm* callbackForm = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &controlCode, &maxInterval, &callbackForm)) {
        DTH_Log("RegisterControlDoubleTap: Failed to extract args");
        return true;
    }

    if (!callbackForm || *((UInt8*)callbackForm + 4) != kFormType_Script) {
        DTH_Log("RegisterControlDoubleTap: Invalid callback");
        return true;
    }

    DoubleTapHandler handler;
    handler.id = g_nextHandlerId++;
    handler.key = controlCode;
    handler.maxInterval = maxInterval;
    handler.useControlCode = true;
    handler.callback = (Script*)callbackForm;

    g_handlers.push_back(handler);
    *result = handler.id;

    DTH_Log("RegisterControlDoubleTap: id=%d control=%d maxInterval=%.2f",
            handler.id, controlCode, maxInterval);
    return true;
}

static ParamInfo kParams_UnregisterDoubleTap[1] = {
    {"handlerID", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(UnregisterKeyDoubleTap,
    "Unregisters a key double-tap handler by ID.",
    0, 1, kParams_UnregisterDoubleTap);

bool Cmd_UnregisterKeyDoubleTap_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 handlerId = 0;
    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &handlerId)) {
        return true;
    }

    for (auto it = g_handlers.begin(); it != g_handlers.end(); ++it) {
        if (it->id == handlerId && !it->useControlCode) {
            g_handlers.erase(it);
            *result = 1;
            DTH_Log("UnregisterKeyDoubleTap: removed id=%d", handlerId);
            return true;
        }
    }

    DTH_Log("UnregisterKeyDoubleTap: id=%d not found", handlerId);
    return true;
}

DEFINE_COMMAND_PLUGIN(UnregisterControlDoubleTap,
    "Unregisters a control double-tap handler by ID.",
    0, 1, kParams_UnregisterDoubleTap);

bool Cmd_UnregisterControlDoubleTap_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 handlerId = 0;
    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &handlerId)) {
        return true;
    }

    for (auto it = g_handlers.begin(); it != g_handlers.end(); ++it) {
        if (it->id == handlerId && it->useControlCode) {
            g_handlers.erase(it);
            *result = 1;
            DTH_Log("UnregisterControlDoubleTap: removed id=%d", handlerId);
            return true;
        }
    }

    DTH_Log("UnregisterControlDoubleTap: id=%d not found", handlerId);
    return true;
}

bool DTH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\DoubleTapHandler.log");
    g_dthLogFile = fopen(logPath, "w");

    DTH_Log("DoubleTapHandler initializing...");

    g_dthScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_dthScript) {
        DTH_Log("ERROR: Failed to get script interface");
        return false;
    }
    g_ExtractArgsEx = g_dthScript->ExtractArgsEx;

    nvse->SetOpcodeBase(0x400E);
    nvse->RegisterCommand(&kCommandInfo_RegisterKeyDoubleTap);
    nvse->SetOpcodeBase(0x400F);
    nvse->RegisterCommand(&kCommandInfo_RegisterControlDoubleTap);
    nvse->SetOpcodeBase(0x4010);
    nvse->RegisterCommand(&kCommandInfo_UnregisterKeyDoubleTap);
    nvse->SetOpcodeBase(0x4011);
    nvse->RegisterCommand(&kCommandInfo_UnregisterControlDoubleTap);

    DTH_Log("Registered commands at opcodes 0x3B0F-0x3B12");
    DTH_Log("DoubleTapHandler initialized successfully");
    return true;
}
