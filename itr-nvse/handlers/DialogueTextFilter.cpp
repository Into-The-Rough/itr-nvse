//dialogue text filter

#include <vector>
#include <string>
#include <cstring>
#include <Windows.h>

#include "DialogueTextFilter.h"
#include "internal/StringMatch.h"
#include "internal/NVSEMinimal.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include <cstdio>

class TESTopicInfo;
class TESTopic;
class TESQuest;

struct String {
    char*   m_data;
    UInt16  m_dataLen;
    UInt16  m_bufLen;

    const char* CStr() const { return m_data ? m_data : ""; }
};

struct TESTopicInfoResponse {
    UInt8   pad00[0x18];
    String  responseText;           //0x18
    String  voiceFilePath;          //0x20
    TESTopicInfoResponse* next;     //0x28
};
static_assert(offsetof(TESTopicInfoResponse, responseText) == 0x18);

struct ModInfo {
    UInt8 pad00[0x20];
    char name[0x100];
};

template <typename T, typename... Args>
__forceinline T ThisStdCall(UInt32 addr, void* thisPtr, Args... args) {
    return ((T(__thiscall*)(void*, Args...))addr)(thisPtr, args...);
}

static PluginHandle g_dtfPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_dtfScript = nullptr;
static _CaptureLambdaVars g_CaptureLambdaVars = nullptr;
static _UncaptureLambdaVars g_UncaptureLambdaVars = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_dtfOpcode = 0;

struct TextFilterEntry {
    char*   filterText;
    Script* callback;

    TextFilterEntry(const char* text, Script* script)
        : filterText(nullptr), callback(script)
    {
        if (text) {
            size_t len = strlen(text) + 1;
            filterText = new char[len];
            memcpy(filterText, text, len);
        }
    }

    ~TextFilterEntry() {
        delete[] filterText;
        filterText = nullptr;
    }

    TextFilterEntry(TextFilterEntry&& other) noexcept
        : filterText(other.filterText), callback(other.callback)
    {
        other.filterText = nullptr;
        other.callback = nullptr;
    }

    TextFilterEntry& operator=(TextFilterEntry&& other) noexcept {
        if (this != &other) {
            delete[] filterText;
            filterText = other.filterText;
            callback = other.callback;
            other.filterText = nullptr;
            other.callback = nullptr;
        }
        return *this;
    }

    TextFilterEntry(const TextFilterEntry&) = delete;
    TextFilterEntry& operator=(const TextFilterEntry&) = delete;

    bool Matches(const char* responseText) const {
        return StringMatch::ContainsSubstringCI(responseText, filterText);
    }
};

struct QueuedDialogueEvent
{
    UInt32 speakerRefID;
    UInt32 topicInfoRefID;
    UInt32 topicRefID;
    float duration;
    char text[512];
    char voicePath[512];
};

namespace DialogueTextFilter {
    std::vector<TextFilterEntry> g_filters;
    std::vector<DTF_NativeCallback> g_nativeCallbacks;
    std::vector<QueuedDialogueEvent> g_pendingEvents;
    bool g_hookInstalled = false;
    DWORD g_mainThreadId = 0;
    UInt32 g_droppedEvents = 0;
    DWORD g_lastDropLogTick = 0;
}

static CRITICAL_SECTION g_dtfStateLock;
static volatile LONG g_dtfStateLockInit = 0;

static void EnsureStateLockInitialized()
{
    InitCriticalSectionOnce(&g_dtfStateLockInit, &g_dtfStateLock);
}

constexpr UInt32 kAddr_RunResult = 0x61F170;
constexpr UInt32 kAddr_RunResultBody = 0x61F176;
constexpr UInt32 kAddr_GetResponses = 0x61E780;

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
}


static const char* GetModName(UInt8 modIndex) {
    void* dh = *(void**)0x11C3F2C;
    if (!dh || modIndex >= 0xFF) return nullptr;
    ModInfo* modInfo = *(ModInfo**)((UInt8*)dh + 0x21C + (modIndex * 4));
    if (!modInfo) return nullptr;
    return (const char*)((UInt8*)modInfo + 0x20);
}

static const char* GetFormEditorID(void* form) {
    if (!form) return nullptr;
    void** vtable = *(void***)form;
    if (!vtable) return nullptr;
    typedef const char* (__thiscall *GetEditorIDFn)(void*);
    GetEditorIDFn fn = (GetEditorIDFn)vtable[0x4C];
    return fn ? fn(form) : nullptr;
}

static void* GetActorVoiceType(Actor* actor) {
    if (!actor) return nullptr;
    void* baseForm = *(void**)((UInt8*)actor + 0x20);
    if (!baseForm) return nullptr;

    void* voiceType = *(void**)((UInt8*)baseForm + 0x50);
    if (voiceType) {
        UInt8 vtTypeID = *(UInt8*)((UInt8*)voiceType + 0x04);
        if (vtTypeID == 0x5D) return voiceType;
    }

    voiceType = *(void**)((UInt8*)baseForm + 0x94);
    if (voiceType) {
        UInt8 vtTypeID = *(UInt8*)((UInt8*)voiceType + 0x04);
        if (vtTypeID == 0x5D) return voiceType;
    }

    return nullptr;
}

static const char* GetVoiceTypeEditorID(void* voiceType) {
    if (!voiceType) return nullptr;
    String* editorIDStr = (String*)((UInt8*)voiceType + 0x1C);
    return editorIDStr ? editorIDStr->CStr() : nullptr;
}

static bool BuildVoicePath(char* outPath, size_t outSize,
                          TESTopicInfo* topicInfo, Actor* speaker,
                          TESTopicInfoResponse* response)
{
    if (!outPath || !topicInfo || !speaker) return false;
    outPath[0] = '\0';

    UInt32 formID = *(UInt32*)((UInt8*)topicInfo + 0x0C);
    UInt8 modIndex = (UInt8)(formID >> 24);
    UInt32 baseFormID = formID & 0x00FFFFFF;

    const char* modName = GetModName(modIndex);
    if (!modName || !*modName) return false;

    void* voiceType = GetActorVoiceType(speaker);
    const char* voiceTypeID = voiceType ? GetVoiceTypeEditorID(voiceType) : nullptr;
    if (!voiceTypeID || !*voiceTypeID) return false;

    void* quest = *(void**)((UInt8*)topicInfo + 0x48);
    const char* questID = GetFormEditorID(quest);
    if (!questID) questID = "";

    void* topic = *(void**)((UInt8*)topicInfo + 0x50);
    const char* topicID = GetFormEditorID(topic);
    if (!topicID) topicID = "";

    UInt8 responseNum = 1;
    if (response) {
        responseNum = *(UInt8*)((UInt8*)response + 0x0C);
        if (responseNum == 0) responseNum = 1;
    }

    sprintf_s(outPath, outSize,
              "Data\\Sound\\Voice\\%s\\%s\\%s_%s_%08X_%u.ogg",
              modName, voiceTypeID, questID, topicID, baseFormID, responseNum);

    return true;
}

static bool IsValidFormPointer(void* form) {
    if (!form) return false;
    //SEH required: called from AI thread where form pointers can be dangling
    __try {
        UInt32 refID = *(UInt32*)((UInt8*)form + 0x0C);
        UInt32 flags = *(UInt32*)((UInt8*)form + 0x08);
        if (refID == 0) return false;
        if (flags & 0x20) return false;  //kFormFlags_Deleted
        if (flags & 0x800) return false; //kFormFlags_Temporary
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void __cdecl HookCallback(TESTopicInfo* topicInfo, Actor* speaker) {

    if (!IsValidFormPointer(topicInfo) || !IsValidFormPointer(speaker)) {
        return;
    }

    TESTopicInfoResponse** ppResponse = ThisStdCall<TESTopicInfoResponse**>(
        kAddr_GetResponses, topicInfo, nullptr);

    if (!ppResponse || !*ppResponse) {
        return;
    }

    TESTopic* topic = *(TESTopic**)((UInt8*)topicInfo + 0x50);

    TESTopicInfoResponse* response = *ppResponse;
    const char* text = response ? response->responseText.CStr() : nullptr;

    if (!text || !*text) {
        return;
    }

    //strlen * fNoticeTextTimePerCharacter (setting at 0x11D2178, float at +0x04)
    float timePerChar = *(float*)(0x11D2178 + 0x04);
    if (timePerChar <= 0.0f) timePerChar = 0.08f;
    float duration = (float)strlen(text) * timePerChar;
    if (duration < 2.0f) duration = 2.0f;

    QueuedDialogueEvent evt{};
    evt.speakerRefID = ReadRefID(speaker);
    evt.topicInfoRefID = ReadRefID(topicInfo);
    evt.topicRefID = ReadRefID(topic);
    evt.duration = duration;
    strncpy_s(evt.text, sizeof(evt.text), text, _TRUNCATE);
    evt.voicePath[0] = '\0';

    EnsureStateLockInitialized();
    {
        ScopedLock lock(&g_dtfStateLock);
        if (DialogueTextFilter::g_nativeCallbacks.empty() &&
            DialogueTextFilter::g_filters.empty()) {
            return;
        }

        constexpr size_t kMaxQueuedDialogueEvents = 256;
        if (DialogueTextFilter::g_pendingEvents.size() >= kMaxQueuedDialogueEvents) {
            ++DialogueTextFilter::g_droppedEvents;
            return;
        }

        DialogueTextFilter::g_pendingEvents.push_back(evt);
    }
}

static auto g_hookCallback = &HookCallback;
static UInt32 g_chainAddr = 0;

static __declspec(naked) void DialogueTextHook() {
    __asm {
        cmp     dword ptr [esp+4], 0
        jnz     skip_filter

        pushad
        pushfd

        //speaker at [esp+8] before pushad(0x20)+pushfd(0x4)
        push    dword ptr [esp+0x2C]
        push    ecx
        call    [g_hookCallback]
        add     esp, 8

        popfd
        popad

    skip_filter:
        mov     eax, [g_chainAddr]
        test    eax, eax
        jnz     chain_to_previous

        push    ebp
        mov     ebp, esp
        sub     esp, 0Ch
        mov     eax, kAddr_RunResultBody
        jmp     eax

    chain_to_previous:
        jmp     eax
    }
}

static void InitHook() {
    if (DialogueTextFilter::g_hookInstalled) return;

    UInt8 firstByte = *(UInt8*)kAddr_RunResult;
    if (firstByte == 0xE9) {
        g_chainAddr = SafeWrite::GetRelJumpTarget(kAddr_RunResult);
    } else {
        g_chainAddr = 0;
    }

    SafeWrite::WriteRelJump(kAddr_RunResult, (UInt32)DialogueTextHook);
    SafeWrite::Write8(kAddr_RunResult + 5, 0x90);

    DialogueTextFilter::g_hookInstalled = true;
}

static bool AddFilter_Internal(const char* filterText, Script* callback) {
    if (!filterText || !callback) return false;

    if (g_CaptureLambdaVars) {
        g_CaptureLambdaVars(callback);
    }

    bool installHook = false;
    EnsureStateLockInitialized();
    {
        ScopedLock lock(&g_dtfStateLock);
        DialogueTextFilter::g_filters.emplace_back(filterText, callback);
        installHook = !DialogueTextFilter::g_hookInstalled;
    }

    if (installHook) {
        InitHook();
    }

    return true;
}

static bool RemoveFilter_Internal(const char* filterText, Script* callback) {
    if (!filterText || !callback) return false;

    bool removed = false;
    EnsureStateLockInitialized();
    {
        ScopedLock lock(&g_dtfStateLock);
        for (auto it = DialogueTextFilter::g_filters.begin(); it != DialogueTextFilter::g_filters.end(); ++it) {
            if (it->callback == callback &&
                it->filterText &&
                _stricmp(it->filterText, filterText) == 0)
            {
                DialogueTextFilter::g_filters.erase(it);
                removed = true;
                break;
            }
        }
    }

    if (removed)
    {
        if (g_UncaptureLambdaVars) {
            g_UncaptureLambdaVars(callback);
        }

        return true;
    }

    return false;
}

static ParamInfo kParams_DialogueTextHandler[3] = {
    {"filterText", kParamType_String, 0},
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnDialogueTextEventHandler,
    "Registers/unregisters a callback for dialogue containing specific text",
    0, 3, kParams_DialogueTextHandler);

bool Cmd_SetOnDialogueTextEventHandler_Execute(COMMAND_ARGS) {
    *result = 0;

    char filterText[512] = {0};
    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            filterText,
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
        if (AddFilter_Internal(filterText, callback)) {
            *result = 1;
        }
    } else {
        if (RemoveFilter_Internal(filterText, callback)) {
            *result = 1;
        }
    }

    return true;
}

bool DTF_Init(void* nvseInterface) {
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;
    EnsureStateLockInitialized();
    DialogueTextFilter::g_mainThreadId = 0;
    DialogueTextFilter::g_lastDropLogTick = GetTickCount();

    g_dtfPluginHandle = nvse->GetPluginHandle();

    g_dtfScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_dtfScript) {
        return false;
    }

    g_ExtractArgsEx = g_dtfScript->ExtractArgsEx;

    NVSEDataInterface* nvseData = reinterpret_cast<NVSEDataInterface*>(
        nvse->QueryInterface(kInterface_Data));

    if (nvseData) {
        g_CaptureLambdaVars = reinterpret_cast<_CaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList));
        g_UncaptureLambdaVars = reinterpret_cast<_UncaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList));
    }

    nvse->SetOpcodeBase(0x4000);
    nvse->RegisterCommand(&kCommandInfo_SetOnDialogueTextEventHandler);
    g_dtfOpcode = 0x4000;

    return true;
}

bool DTF_AddFilter(const char* filterText, void* callback) {
    return AddFilter_Internal(filterText, reinterpret_cast<Script*>(callback));
}

bool DTF_RemoveFilter(const char* filterText, void* callback) {
    return RemoveFilter_Internal(filterText, reinterpret_cast<Script*>(callback));
}

unsigned int DTF_GetOpcode() {
    return g_dtfOpcode;
}

void DTF_Update()
{
    if (!g_dtfScript) return;
    EnsureStateLockInitialized();

    DWORD now = GetTickCount();
    DWORD currentThreadId = GetCurrentThreadId();
    if (!DialogueTextFilter::g_mainThreadId)
        DialogueTextFilter::g_mainThreadId = currentThreadId;
    if (currentThreadId != DialogueTextFilter::g_mainThreadId)
        return;

    UInt32 droppedToLog = 0;
    std::vector<QueuedDialogueEvent> pendingEvents;
    {
        ScopedLock lock(&g_dtfStateLock);
        pendingEvents.swap(DialogueTextFilter::g_pendingEvents);

        if (DialogueTextFilter::g_droppedEvents &&
            (now - DialogueTextFilter::g_lastDropLogTick) >= 5000)
        {
            droppedToLog = DialogueTextFilter::g_droppedEvents;
            DialogueTextFilter::g_droppedEvents = 0;
            DialogueTextFilter::g_lastDropLogTick = now;
        }
    }

    for (const auto& evt : pendingEvents) {
        std::vector<DTF_NativeCallback> nativeCallbacks;
        std::vector<Script*> matchedScriptCallbacks;
        {
            ScopedLock lock(&g_dtfStateLock);
            nativeCallbacks = DialogueTextFilter::g_nativeCallbacks;
            matchedScriptCallbacks.reserve(DialogueTextFilter::g_filters.size());
            for (const auto& filter : DialogueTextFilter::g_filters) {
                if (filter.Matches(evt.text) && filter.callback) {
                    matchedScriptCallbacks.push_back(filter.callback);
                }
            }
        }

        Actor* speaker = reinterpret_cast<Actor*>(Engine::LookupFormByID(evt.speakerRefID));
        TESTopicInfo* topicInfo = reinterpret_cast<TESTopicInfo*>(Engine::LookupFormByID(evt.topicInfoRefID));
        TESTopic* topic = reinterpret_cast<TESTopic*>(Engine::LookupFormByID(evt.topicRefID));

        for (auto callback : nativeCallbacks) {
            if (callback) {
                callback(speaker, evt.text, evt.duration, topicInfo, topic);
            }
        }

        if (g_eventManagerInterface && speaker && topicInfo) {
            char evtVoicePath[512] = {0};
            TESTopicInfoResponse** ppR = ThisStdCall<TESTopicInfoResponse**>(
                kAddr_GetResponses, topicInfo, nullptr);
            TESTopicInfoResponse* r = (ppR && *ppR) ? *ppR : nullptr;
            if (!BuildVoicePath(evtVoicePath, sizeof(evtVoicePath), topicInfo, speaker, r))
                evtVoicePath[0] = '\0';
            g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
                reinterpret_cast<TESObjectREFR*>(speaker),
                speaker, topic, topicInfo, evt.text, evtVoicePath);
        }

        if (!matchedScriptCallbacks.empty() && topicInfo && speaker) {
            char voicePath[512] = {0};
            TESTopicInfoResponse** ppResp = ThisStdCall<TESTopicInfoResponse**>(
                kAddr_GetResponses, topicInfo, nullptr);
            TESTopicInfoResponse* resp = (ppResp && *ppResp) ? *ppResp : nullptr;
            if (!BuildVoicePath(voicePath, sizeof(voicePath), topicInfo, speaker, resp))
                voicePath[0] = '\0';

            for (Script* callback : matchedScriptCallbacks) {
                if (callback) {
                    g_dtfScript->CallFunctionAlt(
                        callback,
                        reinterpret_cast<TESObjectREFR*>(speaker),
                        5,
                        speaker,
                        topic,
                        topicInfo,
                        evt.text,
                        voicePath
                    );
                }
            }
        }
    }
}

//native callback registration for inter-plugin communication
extern "C" {

__declspec(dllexport) bool DTF_RegisterNativeCallback(DTF_NativeCallback callback) {
    if (!callback) return false;

    EnsureStateLockInitialized();
    int callbackCount = 0;
    bool installHook = false;
    {
        ScopedLock lock(&g_dtfStateLock);
        for (auto cb : DialogueTextFilter::g_nativeCallbacks) {
            if (cb == callback) return true;
        }

        DialogueTextFilter::g_nativeCallbacks.push_back(callback);
        callbackCount = (int)DialogueTextFilter::g_nativeCallbacks.size();
        installHook = !DialogueTextFilter::g_hookInstalled;
    }

    if (installHook) {
        InitHook();
    }

    return true;
}

__declspec(dllexport) bool DTF_UnregisterNativeCallback(DTF_NativeCallback callback) {
    if (!callback) return false;

    EnsureStateLockInitialized();
    {
        ScopedLock lock(&g_dtfStateLock);
        auto& callbacks = DialogueTextFilter::g_nativeCallbacks;
        for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
            if (*it == callback) {
                callbacks.erase(it);
                return true;
            }
        }
    }
    return false;
}

}

void DTF_ClearCallbacks()
{
    //native callbacks persist across game loads (other plugins own them)
    EnsureStateLockInitialized();
    {
        ScopedLock lock(&g_dtfStateLock);
        DialogueTextFilter::g_filters.clear();
        DialogueTextFilter::g_pendingEvents.clear();
        DialogueTextFilter::g_droppedEvents = 0;
    }
}
