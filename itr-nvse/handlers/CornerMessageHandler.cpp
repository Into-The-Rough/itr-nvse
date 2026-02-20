//corner message event handler - fires when HUD notification displays
//hooks earlier than SetShowOffOnCornerMessageEventHandler to catch ALL messages including first on load

#include <vector>
#include <string>
#include <Windows.h>
#include <cmath>

#include "CornerMessageHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/ScopedLock.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"

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
    int metaType;
};

static std::vector<QueuedMessage> g_queuedMessages;
static bool g_hasHandlers = false;

struct PendingMetaEntry {
    std::string text;
    float displayTime;
    int metaType;
    DWORD tick;
};

static std::vector<PendingMetaEntry> g_pendingMeta;
static CRITICAL_SECTION g_metaLock;
static volatile LONG g_metaLockInit = 0;

static void EnsureMetaLockInitialized()
{
    InitCriticalSectionOnce(&g_metaLockInit, &g_metaLock);
}

static int NormalizeMetaType(int metaType)
{
    if (metaType < kCornerMeta_Generic || metaType > kCornerMeta_ReputationChange)
        return kCornerMeta_Generic;
    return metaType;
}

static int ConsumeMessageMeta(const char* text, float displayTime)
{
    if (g_metaLockInit != 2 || !text || !text[0]) {
        return kCornerMeta_Generic;
    }

    const DWORD now = GetTickCount();
    constexpr DWORD kMetaTtlMs = 60000;
    constexpr float kDisplayTimeTolerance = 0.25f;

    ScopedLock lock(&g_metaLock);

    for (auto it = g_pendingMeta.begin(); it != g_pendingMeta.end();) {
        if ((now - it->tick) > kMetaTtlMs) {
            it = g_pendingMeta.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = g_pendingMeta.begin(); it != g_pendingMeta.end(); ++it) {
        if (it->text == text && std::fabs(it->displayTime - displayTime) <= kDisplayTimeTolerance) {
            int metaType = it->metaType;
            g_pendingMeta.erase(it);
            return metaType;
        }
    }

    return kCornerMeta_Generic;
}

void CMH_TrackMessageMeta(const char* text, float displayTime, int metaType)
{
    if (!text || !text[0]) return;
    EnsureMetaLockInitialized();

    PendingMetaEntry entry;
    entry.text = text;
    entry.displayTime = displayTime;
    entry.metaType = NormalizeMetaType(metaType);
    entry.tick = GetTickCount();

    ScopedLock lock(&g_metaLock);
    constexpr size_t kMaxMetaEntries = 256;
    if (g_pendingMeta.size() >= kMaxMetaEntries) {
        g_pendingMeta.erase(g_pendingMeta.begin());
    }
    g_pendingMeta.push_back(entry);
}

//HUDMainMenu::ShowNotify at 0x775380
//bool __thiscall ShowNotify(const char* text, eEmotion emotion,
//    const char* iconPath, const char* soundName, float time, bool instant)
constexpr UInt32 kAddr_HUDMainMenu_ShowNotify = 0x775380;
static Detours::JumpDetour s_detour;

static void DispatchCornerMessage(const char* text, UInt32 emotion,
    const char* iconPath, const char* soundPath, float displayTime, int metaType)
{
    const char* safeText = text ? text : "";
    const char* safeIcon = iconPath ? iconPath : "";
    const char* safeSound = soundPath ? soundPath : "";

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnCornerMessage", nullptr,
            safeText, (int)emotion, safeIcon, safeSound, (double)displayTime, metaType);

    if (!g_cmhScript) return;

    for (const auto& cb : g_callbacks) {
        if (cb.script) {
            g_cmhScript->CallFunctionAlt(cb.script, nullptr, 6,
                safeText, emotion, safeIcon, safeSound, *(UInt32*)&displayTime, metaType);
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
    int metaType = ConsumeMessageMeta(text, displayTime);

    if (text && text[0]) {
        if (g_hasHandlers) {
            DispatchCornerMessage(text, emotion, iconPath, soundPath, displayTime, metaType);
        } else {
            QueuedMessage msg;
            msg.text = text;
            msg.emotion = emotion;
            msg.iconPath = iconPath ? iconPath : "";
            msg.soundPath = soundPath ? soundPath : "";
            msg.displayTime = displayTime;
            msg.instant = instant;
            msg.metaType = metaType;
            g_queuedMessages.push_back(msg);
        }
    }

    typedef bool(__thiscall* ShowNotify_t)(void*, const char*, UInt32, const char*, const char*, float, bool);
    return s_detour.GetTrampoline<ShowNotify_t>()(thisPtr, text, emotion, iconPath, soundPath, displayTime, instant);
}

//prologue: push ebp; mov ebp, esp; push -1 = 5 bytes
static void InstallHook() {
    if (!s_detour.WriteRelJump(kAddr_HUDMainMenu_ShowNotify, Hook_HUDMainMenu_ShowNotify, 5)) {
        return;
    }
}

static ParamInfo kParams_SetCornerMessageHandler[2] = {
    {"setOrRemove", kParamType_Integer, 0},
    {"handler",     kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(SetCornerMessageHandler,
    "Registers/unregisters callback for corner message events. Callback args: text, emotion, iconPath, soundPath, displayTime, metaType",
    0, 2, kParams_SetCornerMessageHandler);

bool Cmd_SetCornerMessageHandler_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 setOrRemove = 0;
    TESForm* handlerForm = nullptr;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &setOrRemove, &handlerForm)) {
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
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

            if (!g_queuedMessages.empty()) {
                for (const auto& msg : g_queuedMessages) {
                    if (g_cmhScript) {
                        g_cmhScript->CallFunctionAlt(script, nullptr, 6,
                            msg.text.c_str(), msg.emotion, msg.iconPath.c_str(),
                            msg.soundPath.c_str(), *(UInt32*)&msg.displayTime, msg.metaType);
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

    g_cmhScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_cmhScript) {
        return false;
    }
    g_ExtractArgsEx = g_cmhScript->ExtractArgsEx;
    EnsureMetaLockInitialized();

    InstallHook();

    nvse->SetOpcodeBase(0x4013);
    nvse->RegisterCommand(&kCommandInfo_SetCornerMessageHandler);
    g_registeredOpcode = 0x4013;

    return true;
}

void CMH_ClearCallbacks()
{
    g_callbacks.clear();
    g_queuedMessages.clear();
    g_hasHandlers = false;
    if (g_metaLockInit == 2) {
        ScopedLock lock(&g_metaLock);
        g_pendingMeta.clear();
    }
}
