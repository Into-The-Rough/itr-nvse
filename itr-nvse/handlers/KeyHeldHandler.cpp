//key held event system - fires continuously while key held past threshold

#include <vector>
#include <unordered_map>
#include <Windows.h>

#include "KeyHeldHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"

static NVSEScriptInterface* g_khhScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_nextHandlerId = 1;
static DWORD g_lastTickCount = 0;
static float g_currentTime = 0.0f;

struct KeyHeldHandler {
    UInt32 id;
    UInt32 key;
    float holdThreshold;
    float repeatInterval;
    bool useControlCode;
    Script* callback;
};

struct KeyState {
    bool isDown;
    float downTime;
    float lastDispatch;
};

static std::vector<KeyHeldHandler> g_handlers;
static std::unordered_map<UInt32, KeyState> g_keyStates;

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

static bool IsControlPressed(UInt32 controlCode) {
    if (!g_OSInputGlobals) return false;
    return Engine::OSInputGlobals_GetControlState(g_OSInputGlobals, controlCode, 0);
}

static void DispatchHeldEvent(Script* callback, UInt32 key, float duration) {
    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnKeyHeld", nullptr, (int)key, (double)duration);
    if (!g_khhScript || !callback) return;
    //pass float by reinterpreting raw bits as UInt32 to avoid varargs double promotion
    g_khhScript->CallFunctionAlt(callback, nullptr, 2, key, *(UInt32*)&duration);
}

void KHH_Update() {
    DWORD currentTick = GetTickCount();
    if (g_lastTickCount == 0) g_lastTickCount = currentTick;
    float deltaTime = (currentTick - g_lastTickCount) / 1000.0f;
    g_lastTickCount = currentTick;
    g_currentTime += deltaTime;

    if (!g_OSInputGlobals)
        g_OSInputGlobals = *(void**)0x11F35CC;

    for (const auto& handler : g_handlers) {
        bool keyDown = handler.useControlCode
            ? IsControlPressed(handler.key)
            : IsRawKeyPressed(handler.key);

        KeyState& state = g_keyStates[handler.key];

        if (keyDown && !state.isDown) {
            state.isDown = true;
            state.downTime = g_currentTime;
            state.lastDispatch = 0;
        }
        else if (keyDown && state.isDown) {
            float heldDuration = g_currentTime - state.downTime;

            if (heldDuration >= handler.holdThreshold) {
                bool shouldDispatch = false;

                if (handler.repeatInterval <= 0) {
                    shouldDispatch = true;
                } else if (state.lastDispatch == 0) {
                    shouldDispatch = true;
                } else if (g_currentTime - state.lastDispatch >= handler.repeatInterval) {
                    shouldDispatch = true;
                }

                if (shouldDispatch) {
                    DispatchHeldEvent(handler.callback, handler.key, heldDuration);
                    state.lastDispatch = g_currentTime;
                }
            }
        }
        else if (!keyDown && state.isDown) {
            state.isDown = false;
        }
    }
}

static ParamInfo kParams_RegisterKeyHeld[4] = {
    {"keycode",   kParamType_Integer, 0},
    {"threshold", kParamType_Float,   0},
    {"interval",  kParamType_Float,   0},
    {"callback",  kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(RegisterKeyHeld,
    "Registers callback for key held events. Returns handler ID.",
    0, 4, kParams_RegisterKeyHeld);

bool Cmd_RegisterKeyHeld_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 keycode = 0;
    float threshold = 0, interval = 0;
    TESForm* callbackForm = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &keycode, &threshold, &interval, &callbackForm)) {
        return true;
    }

    if (!callbackForm || *((UInt8*)callbackForm + 4) != kFormType_Script) {
        return true;
    }

    KeyHeldHandler handler;
    handler.id = g_nextHandlerId++;
    handler.key = keycode;
    handler.holdThreshold = threshold;
    handler.repeatInterval = interval;
    handler.useControlCode = false;
    handler.callback = (Script*)callbackForm;

    g_handlers.push_back(handler);
    *result = handler.id;

    return true;
}

DEFINE_COMMAND_PLUGIN(RegisterControlHeld,
    "Registers callback for control held events. Returns handler ID.",
    0, 4, kParams_RegisterKeyHeld);

bool Cmd_RegisterControlHeld_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 controlCode = 0;
    float threshold = 0, interval = 0;
    TESForm* callbackForm = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &controlCode, &threshold, &interval, &callbackForm)) {
        return true;
    }

    if (!callbackForm || *((UInt8*)callbackForm + 4) != kFormType_Script) {
        return true;
    }

    KeyHeldHandler handler;
    handler.id = g_nextHandlerId++;
    handler.key = controlCode;
    handler.holdThreshold = threshold;
    handler.repeatInterval = interval;
    handler.useControlCode = true;
    handler.callback = (Script*)callbackForm;

    g_handlers.push_back(handler);
    *result = handler.id;

    return true;
}

static ParamInfo kParams_UnregisterHeld[1] = {
    {"handlerID", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(UnregisterKeyHeld,
    "Unregisters a key held handler by ID.",
    0, 1, kParams_UnregisterHeld);

bool Cmd_UnregisterKeyHeld_Execute(COMMAND_ARGS) {
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
            return true;
        }
    }

    return true;
}

DEFINE_COMMAND_PLUGIN(UnregisterControlHeld,
    "Unregisters a control held handler by ID.",
    0, 1, kParams_UnregisterHeld);

bool Cmd_UnregisterControlHeld_Execute(COMMAND_ARGS) {
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
            return true;
        }
    }

    return true;
}

bool KHH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    g_khhScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_khhScript) {
        return false;
    }
    g_ExtractArgsEx = g_khhScript->ExtractArgsEx;

    nvse->SetOpcodeBase(0x400A);
    nvse->RegisterCommand(&kCommandInfo_RegisterKeyHeld);
    nvse->SetOpcodeBase(0x400B);
    nvse->RegisterCommand(&kCommandInfo_RegisterControlHeld);
    nvse->SetOpcodeBase(0x400C);
    nvse->RegisterCommand(&kCommandInfo_UnregisterKeyHeld);
    nvse->SetOpcodeBase(0x400D);
    nvse->RegisterCommand(&kCommandInfo_UnregisterControlHeld);

    return true;
}

void KHH_ClearCallbacks()
{
    g_handlers.clear();
    g_keyStates.clear();
}
