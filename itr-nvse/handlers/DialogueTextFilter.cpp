//dialogue text filter

#include <vector>
#include <cstring>
#include <cstdio>
#include <Windows.h>

#include "DialogueTextFilter.h"
#include "internal/StringMatch.h"
#include "internal/NVSEMinimal.h"

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

struct ModInfo {
    UInt8 pad00[0x20];
    char name[0x100];
};

template <typename T, typename... Args>
__forceinline T ThisStdCall(UInt32 addr, void* thisPtr, Args... args) {
    return ((T(__thiscall*)(void*, Args...))addr)(thisPtr, args...);
}

static FILE* g_dtfLogFile = nullptr;

static void DTF_Log(const char* fmt, ...)
{
    if (!g_dtfLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_dtfLogFile, fmt, args);
    fprintf(g_dtfLogFile, "\n");
    fflush(g_dtfLogFile);
    va_end(args);
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

namespace DialogueTextFilter {
    std::vector<TextFilterEntry> g_filters;
    std::vector<DTF_NativeCallback> g_nativeCallbacks;
    bool g_hookInstalled = false;
}

constexpr UInt32 kAddr_RunResult = 0x61F170;
constexpr UInt32 kAddr_RunResultBody = 0x61F176;
constexpr UInt32 kAddr_GetResponses = 0x61E780;

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
    __try {
        UInt32 refID = *(UInt32*)((UInt8*)form + 0x0C);
        UInt32 flags = *(UInt32*)((UInt8*)form + 0x08);
        if (refID == 0) return false;
        if (flags & 0x20) return false;  //deleted
        if (flags & 0x800) return false; //temporary/invalid
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void __cdecl HookCallback(TESTopicInfo* topicInfo, Actor* speaker) {
    DTF_Log("=== HOOK FIRED === topicInfo=0x%08X speaker=0x%08X", topicInfo, speaker);

    if (!IsValidFormPointer(topicInfo) || !IsValidFormPointer(speaker)) {
        DTF_Log("  Invalid form pointer, skipping");
        return;
    }

    TESTopicInfoResponse** ppResponse = ThisStdCall<TESTopicInfoResponse**>(
        kAddr_GetResponses, topicInfo, nullptr);

    if (!ppResponse || !*ppResponse) {
        DTF_Log("  No responses found");
        return;
    }

    TESTopic* topic = *(TESTopic**)((UInt8*)topicInfo + 0x50);
    UInt32 topicInfoFormID = *(UInt32*)((UInt8*)topicInfo + 0x0C);

    DTF_Log("  FormID: %08X", topicInfoFormID);

    TESTopicInfoResponse* response = *ppResponse;
    const char* text = response ? response->responseText.CStr() : nullptr;

    if (!text || !*text) {
        DTF_Log("  No text");
        return;
    }

    //calculate duration using game's formula: strlen * fNoticeTextTimePerCharacter
    //Setting at 0x11D2178, float value at offset 0x04
    float timePerChar = *(float*)(0x11D2178 + 0x04);
    if (timePerChar <= 0.0f) timePerChar = 0.08f; //fallback
    float duration = (float)strlen(text) * timePerChar;
    if (duration < 2.0f) duration = 2.0f; //minimum 2 seconds

    //always call native callbacks (for other plugins like FloatingSubtitles)
    for (auto callback : DialogueTextFilter::g_nativeCallbacks) {
        if (callback) {
            callback(speaker, text, duration, topicInfo, topic);
        }
    }

    //script filters - check for matches
    if (DialogueTextFilter::g_filters.empty()) return;

    bool anyMatch = false;
    for (const auto& filter : DialogueTextFilter::g_filters) {
        if (filter.Matches(text)) {
            anyMatch = true;
            break;
        }
    }

    if (!anyMatch) {
        DTF_Log("  No filter match for: '%s'", text);
        return;
    }

    static char voicePath[512];
    if (!BuildVoicePath(voicePath, sizeof(voicePath), topicInfo, speaker, response)) {
        voicePath[0] = '\0';
    }

    DTF_Log("  Match found: '%s'", text);

    for (const auto& filter : DialogueTextFilter::g_filters) {
        if (filter.Matches(text)) {
            if (g_dtfScript && filter.callback) {
                g_dtfScript->CallFunctionAlt(
                    filter.callback,
                    reinterpret_cast<TESObjectREFR*>(speaker),
                    5,
                    speaker,
                    topic,
                    topicInfo,
                    text,
                    voicePath
                );
            }
        }
    }
}

static auto g_hookCallback = &HookCallback;
static UInt32 g_chainAddr = 0;

static __declspec(naked) void DialogueTextHook() {
    static UInt32 s_thisPtr;
    static UInt32 s_speakerPtr;
    __asm {
        cmp     dword ptr [esp+4], 0
        jnz     skip_filter

        mov     s_thisPtr, ecx
        mov     eax, [esp+8]
        mov     s_speakerPtr, eax

        pushad
        pushfd

        push    s_speakerPtr
        push    s_thisPtr
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
        DTF_Log("Detected existing hook at 0x%08X, chaining to 0x%08X", kAddr_RunResult, g_chainAddr);
    } else {
        g_chainAddr = 0;
        DTF_Log("No existing hook at 0x%08X", kAddr_RunResult);
    }

    SafeWrite::WriteRelJump(kAddr_RunResult, (UInt32)DialogueTextHook);
    SafeWrite::Write8(kAddr_RunResult + 5, 0x90);

    DialogueTextFilter::g_hookInstalled = true;
    DTF_Log("Hook installed at 0x%08X", kAddr_RunResult);
}

static bool AddFilter_Internal(const char* filterText, Script* callback) {
    if (!filterText || !callback) return false;

    if (g_CaptureLambdaVars) {
        g_CaptureLambdaVars(callback);
    }

    DialogueTextFilter::g_filters.emplace_back(filterText, callback);

    if (!DialogueTextFilter::g_hookInstalled) {
        InitHook();
    }

    DTF_Log("Added filter: '%s' (callback: 0x%08X)", filterText, callback);
    return true;
}

static bool RemoveFilter_Internal(const char* filterText, Script* callback) {
    if (!filterText || !callback) return false;

    for (auto it = DialogueTextFilter::g_filters.begin(); it != DialogueTextFilter::g_filters.end(); ++it) {
        if (it->callback == callback &&
            it->filterText &&
            _stricmp(it->filterText, filterText) == 0)
        {
            if (g_UncaptureLambdaVars) {
                g_UncaptureLambdaVars(it->callback);
            }

            DialogueTextFilter::g_filters.erase(it);
            DTF_Log("Removed filter: '%s'", filterText);
            return true;
        }
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
    DTF_Log("Command called!");

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
        DTF_Log("Failed to extract args");
        return true;
    }

    DTF_Log("Extracted args: filter='%s', callback=0x%08X, add=%d",
             filterText, callbackForm, addRemove);

    if (!callbackForm) {
        DTF_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    DTF_Log("Callback typeID: 0x%02X (expected 0x%02X)", typeID, kFormType_Script);

    if (typeID != kFormType_Script) {
        DTF_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        DTF_Log("Adding filter...");
        if (AddFilter_Internal(filterText, callback)) {
            *result = 1;
            DTF_Log("Filter added successfully");
        } else {
            DTF_Log("Failed to add filter");
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

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\DialogueTextFilter.log");
    g_dtfLogFile = fopen(logPath, "w");

    DTF_Log("DialogueTextFilter module initializing...");

    g_dtfPluginHandle = nvse->GetPluginHandle();

    g_dtfScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_dtfScript) {
        DTF_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_dtfScript->ExtractArgsEx;
    DTF_Log("Script interface at 0x%08X, ExtractArgsEx at 0x%08X",
            g_dtfScript, g_ExtractArgsEx);

    NVSEDataInterface* nvseData = reinterpret_cast<NVSEDataInterface*>(
        nvse->QueryInterface(kInterface_Data));

    if (nvseData) {
        g_CaptureLambdaVars = reinterpret_cast<_CaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList));
        g_UncaptureLambdaVars = reinterpret_cast<_UncaptureLambdaVars>(
            nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList));
        DTF_Log("Lambda capture: save=0x%08X unsave=0x%08X",
                g_CaptureLambdaVars, g_UncaptureLambdaVars);
    }

    nvse->SetOpcodeBase(0x4000);
    nvse->RegisterCommand(&kCommandInfo_SetOnDialogueTextEventHandler);
    g_dtfOpcode = 0x4000;

    DTF_Log("Registered SetOnDialogueTextEventHandler at opcode 0x%04X", g_dtfOpcode);
    DTF_Log("DialogueTextFilter module initialized successfully");

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

//native callback registration for inter-plugin communication
extern "C" {

__declspec(dllexport) bool DTF_RegisterNativeCallback(DTF_NativeCallback callback) {
    if (!callback) return false;

    //check if already registered
    for (auto cb : DialogueTextFilter::g_nativeCallbacks) {
        if (cb == callback) return true;
    }

    DialogueTextFilter::g_nativeCallbacks.push_back(callback);

    //ensure hook is installed
    if (!DialogueTextFilter::g_hookInstalled) {
        InitHook();
    }

    DTF_Log("Registered native callback: 0x%08X (total: %d)",
            callback, DialogueTextFilter::g_nativeCallbacks.size());
    return true;
}

__declspec(dllexport) bool DTF_UnregisterNativeCallback(DTF_NativeCallback callback) {
    if (!callback) return false;

    auto& callbacks = DialogueTextFilter::g_nativeCallbacks;
    for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
        if (*it == callback) {
            callbacks.erase(it);
            DTF_Log("Unregistered native callback: 0x%08X", callback);
            return true;
        }
    }
    return false;
}

}
