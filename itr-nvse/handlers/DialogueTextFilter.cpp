//dialogue text filter - self-contained module (no NVSE-Plugins-main headers)

#include <cstdint>
#include <vector>
#include <cstring>
#include <cstdio>
#include <Windows.h>

#include "DialogueTextFilter.h"
#include "internal/StringMatch.h"


class TESForm;
class TESTopicInfo;
class TESTopic;
class TESQuest;
class Actor;
class Script;
class TESObjectREFR;

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

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

struct CommandInfo;
struct ParamInfo;
struct ScriptEventList;

using PluginHandle = UInt32;
constexpr PluginHandle kPluginHandle_Invalid = 0xFFFFFFFF;

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

enum {
    kInterface_Serialization = 0,
    kInterface_Console,
    kInterface_Messaging,
    kInterface_CommandTable,
    kInterface_StringVar,
    kInterface_ArrayVar,
    kInterface_Script,
    kInterface_Data,
};

struct NVSEArrayVarInterface {
    struct Element {
        UInt8 pad[16];
    };
};

struct NVSEScriptInterface {
    enum { kVersion = 1 };

    bool    (*CallFunction)(Script* funcScript, TESObjectREFR* callingObj,
                TESObjectREFR* container, NVSEArrayVarInterface::Element* result,
                UInt8 numArgs, ...);
    int     (*GetFunctionParams)(Script* funcScript, UInt8* paramTypesOut);
    bool    (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptDataIn,
                UInt32* scriptDataOffset, Script* scriptObj, ScriptEventList* eventList, ...);
    bool    (*ExtractFormatStringArgs)(UInt32 fmtStringPos, char* buffer,
                ParamInfo* paramInfo, void* scriptDataIn, UInt32* scriptDataOffset,
                Script* scriptObj, ScriptEventList* eventList, UInt32 maxParams, ...);
    bool    (*CallFunctionAlt)(Script* funcScript, TESObjectREFR* callingObj,
                UInt8 numArgs, ...);
    Script* (*CompileScript)(const char* scriptText);
    Script* (*CompileExpression)(const char* expression);
    size_t  (__stdcall *pDecompileToBuffer)(Script* pScript, void* pStream, char* pBuffer);
};

struct NVSEDataInterface {
    enum { kVersion = 1 };
    UInt32  version;
    void*   (*GetSingleton)(UInt32 singletonID);
    enum {
        kNVSEData_InventoryReferenceCreate = 1,
        kNVSEData_InventoryReferenceGetForRefID,
        kNVSEData_InventoryReferenceGetRefBySelf,
        kNVSEData_ArrayVarMapDeleteBySelf,
        kNVSEData_StringVarMapDeleteBySelf,
        kNVSEData_LambdaDeleteAllForScript,
        kNVSEData_InventoryReferenceCreateEntry,
        kNVSEData_LambdaSaveVariableList,
        kNVSEData_LambdaUnsaveVariableList,
    };
    void*   (*GetFunc)(UInt32 funcID);
    void*   (*GetData)(UInt32 dataID);
};

using _CaptureLambdaVars = void (*)(Script* scriptLambda);
using _UncaptureLambdaVars = void (*)(Script* scriptLambda);

#define COMMAND_ARGS        void* paramInfo, void* scriptData, TESObjectREFR* thisObj, \
                            UInt32 containingObj, Script* scriptObj, ScriptEventList* eventList, \
                            double* result, UInt32* opcodeOffsetPtr

#define EXTRACT_ARGS_EX     paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

using CommandExecuteFunc = bool (*)(COMMAND_ARGS);
using CommandParseFunc = bool (*)(UInt32, void*, void*, void*);
using CommandEvalFunc = bool (*)(TESObjectREFR*, void*, void*, double*);

struct ParamInfo {
    const char* typeStr;
    UInt32      typeID;
    UInt32      isOptional;
};

struct CommandInfo {
    const char*         longName;
    const char*         shortName;
    UInt32              opcode;
    const char*         helpText;
    UInt16              needsParent;
    UInt16              numParams;
    ParamInfo*          params;
    CommandExecuteFunc  execute;
    CommandParseFunc    parse;
    CommandEvalFunc     eval;
    UInt32              flags;
};

enum ParamType {
    kParamType_String       = 0x00,
    kParamType_Integer      = 0x01,
    kParamType_Float        = 0x02,
    kParamType_AnyForm      = 0x3D,
};

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, \
        params ? (sizeof(params) / sizeof(ParamInfo)) : 0, \
        params, Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

enum FormType {
    kFormType_Script = 0x11,
};

namespace SafeWrite {
    inline void Write8(UInt32 addr, UInt8 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt8*)addr = data;
        VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
    }

    inline void Write32(UInt32 addr, UInt32 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt32*)addr = data;
        VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        Write8(src, 0xE9);
        Write32(src + 1, dst - src - 5);
    }

    inline UInt32 GetRelJumpTarget(UInt32 src) {
        return *(UInt32*)(src + 1) + src + 5;
    }
}

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

static void __cdecl HookCallback(TESTopicInfo* topicInfo, Actor* speaker) {
    DTF_Log("=== HOOK FIRED === topicInfo=0x%08X speaker=0x%08X", topicInfo, speaker);

    if (DialogueTextFilter::g_filters.empty()) return;

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
    0, kParams_DialogueTextHandler);

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

    nvse->SetOpcodeBase(0x3B00);
    nvse->RegisterCommand(&kCommandInfo_SetOnDialogueTextEventHandler);
    g_dtfOpcode = 0x3B00;

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
