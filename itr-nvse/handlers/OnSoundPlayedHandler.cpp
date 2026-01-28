//hooks BSAudioManager::GetSoundHandleByFilePath at 0xAE5A50 to catch ALL sounds
//uses a queue to dispatch events on the main thread (audio may run on separate thread)

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <Windows.h>

#include "OnSoundPlayedHandler.h"


class TESForm;
class TESObjectREFR;
class Script;
class ScriptEventList;
class TESSound;

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

struct CommandInfo;
struct ParamInfo;

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
};

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
    kParamType_Integer      = 0x01,
    kParamType_AnyForm      = 0x3D,
};

enum FormType {
    kFormType_Script = 0x11,
};

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

struct BSSoundHandle
{
    UInt32 uiSoundID;
    UInt8 bAssumeSuccess;
    UInt8 pad[3];
    UInt32 uiState;
};

// Sound flags for categorization
enum SoundFlags
{
    kSoundFlag_2D           = 0x1,
    kSoundFlag_3D           = 0x2,
    kSoundFlag_IsVoice      = 0x4,
    kSoundFlag_IsFootsteps  = 0x8,
    kSoundFlag_Loop         = 0x10,
    kSoundFlag_NotDialogue  = 0x20,
    kSoundFlag_IsMusic      = 0x800,
    kSoundFlag_IsRadio      = 0x100000,
};

// BSAudioManager - minimal definition for GetSingleton
struct BSAudioManager
{
    static BSAudioManager* Get() { return *(BSAudioManager**)0x11F6EF0; }
};

struct QueuedSoundEvent
{
    char filePath[260];
    UInt32 soundFlags;
    TESSound* sourceSound;
};

struct TrackedVoiceSound
{
    UInt32 soundID;
    char filePath[260];
    UInt32 soundFlags;
    TESSound* sourceSound;
};

//BSSoundHandle::IsPlaying - checks if sound is still playing
typedef bool (__thiscall *IsPlaying_t)(void* thisPtr);
static const IsPlaying_t BSSoundHandle_IsPlaying = (IsPlaying_t)0xAD8930;

static FILE* g_osphLogFile = nullptr;

static void OSPH_Log(const char* fmt, ...)
{
    if (!g_osphLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_osphLogFile, fmt, args);
    fprintf(g_osphLogFile, "\n");
    fflush(g_osphLogFile);
    va_end(args);
}

static PluginHandle g_osphPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_osphScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_osphOpcode = 0;

namespace OnSoundPlayedHandler {
    std::vector<Script*> g_callbacks;
    std::vector<Script*> g_completionCallbacks;
    bool g_hookInstalled = false;

    // Thread-safe queue for sound events
    std::vector<QueuedSoundEvent> g_pendingEvents;
    CRITICAL_SECTION g_queueLock;
    bool g_lockInitialized = false;

    // Track playing voice sounds for completion detection
    std::vector<TrackedVoiceSound> g_trackedSounds;
}

// Hook address: BSAudioManager::GetSoundHandleByFilePath
// 0xAE5A50 (decimal: 11426384)
constexpr UInt32 kAddr_GetSoundHandleByFilePath = 0xAE5A50;

// Original function pointer
// BSSoundHandle* __thiscall GetSoundHandleByFilePath(BSAudioManager*, BSSoundHandle*, const char*, UInt32, TESSound*)
typedef BSSoundHandle* (__thiscall* GetSoundHandleByFilePath_t)(
    BSAudioManager* mgr,
    BSSoundHandle* arData,
    const char* apName,
    UInt32 aeAudioFlags,
    TESSound* apSound
);
static GetSoundHandleByFilePath_t OriginalGetSoundHandle = nullptr;

// Trampoline storage
static UInt8* s_trampoline = nullptr;

static const char* GetSoundCategory(UInt32 flags)
{
    if (flags & kSoundFlag_IsVoice) return "Voice";
    if (flags & kSoundFlag_IsMusic) return "Music";
    if (flags & kSoundFlag_IsRadio) return "Radio";
    if (flags & kSoundFlag_IsFootsteps) return "Footsteps";
    if (flags & kSoundFlag_3D) return "3D";
    if (flags & kSoundFlag_2D) return "2D";
    return "Unknown";
}

//queue a sound event (may be called from audio thread - must be thread-safe)
static void QueueSoundEvent(const char* filePath, UInt32 flags, TESSound* sourceSound)
{
    if (!OnSoundPlayedHandler::g_lockInitialized) return;

    QueuedSoundEvent evt;

    // Copy file path
    if (filePath && filePath[0])
    {
        strncpy(evt.filePath, filePath, 259);
        evt.filePath[259] = '\0';
    }
    else
    {
        evt.filePath[0] = '\0';
    }

    evt.soundFlags = flags;
    evt.sourceSound = sourceSound;

    EnterCriticalSection(&OnSoundPlayedHandler::g_queueLock);
    OnSoundPlayedHandler::g_pendingEvents.push_back(evt);
    LeaveCriticalSection(&OnSoundPlayedHandler::g_queueLock);
}

// Queue a voice sound for completion tracking
static void QueueVoiceTracking(UInt32 soundID, const char* filePath, UInt32 flags, TESSound* sourceSound)
{
    if (!OnSoundPlayedHandler::g_lockInitialized) return;
    if (soundID == 0 || soundID == 0xFFFFFFFF) return;

    TrackedVoiceSound tracked;
    tracked.soundID = soundID;
    tracked.soundFlags = flags;
    tracked.sourceSound = sourceSound;

    if (filePath && filePath[0])
    {
        strncpy(tracked.filePath, filePath, 259);
        tracked.filePath[259] = '\0';
    }
    else
    {
        tracked.filePath[0] = '\0';
    }

    EnterCriticalSection(&OnSoundPlayedHandler::g_queueLock);
    OnSoundPlayedHandler::g_trackedSounds.push_back(tracked);
    LeaveCriticalSection(&OnSoundPlayedHandler::g_queueLock);

    OSPH_Log("Tracking voice sound ID=%08X path=%s", soundID, tracked.filePath);
}

// Hooked GetSoundHandleByFilePath function
static BSSoundHandle* __fastcall HookedGetSoundHandle(
    BSAudioManager* mgr, void* edx,
    BSSoundHandle* arData,
    const char* apName,
    UInt32 aeAudioFlags,
    TESSound* apSound)
{
    // Queue event if we have callbacks
    if (!OnSoundPlayedHandler::g_callbacks.empty() && apName && apName[0])
    {
        QueueSoundEvent(apName, aeAudioFlags, apSound);
    }

    // Call original
    BSSoundHandle* result = OriginalGetSoundHandle(mgr, arData, apName, aeAudioFlags, apSound);

    // Track voice sounds for completion detection
    if (!OnSoundPlayedHandler::g_completionCallbacks.empty() &&
        (aeAudioFlags & kSoundFlag_IsVoice) && apName && apName[0])
    {
        if (result && result->uiSoundID != 0 && result->uiSoundID != 0xFFFFFFFF)
        {
            QueueVoiceTracking(result->uiSoundID, apName, aeAudioFlags, apSound);
        }
    }

    return result;
}

void OSPH_Update()
{
    if (!OnSoundPlayedHandler::g_lockInitialized) return;

    // Swap out the pending events under lock
    std::vector<QueuedSoundEvent> eventsToProcess;
    std::vector<TrackedVoiceSound> soundsToCheck;

    EnterCriticalSection(&OnSoundPlayedHandler::g_queueLock);
    eventsToProcess.swap(OnSoundPlayedHandler::g_pendingEvents);
    soundsToCheck = OnSoundPlayedHandler::g_trackedSounds; //copy for checking
    LeaveCriticalSection(&OnSoundPlayedHandler::g_queueLock);

    // Process sound started events
    if (!eventsToProcess.empty() && !OnSoundPlayedHandler::g_callbacks.empty())
    {
        for (const auto& evt : eventsToProcess)
        {
            const char* filePath = evt.filePath[0] ? evt.filePath : "";

            OSPH_Log("Dispatching: Flags=0x%X Category=%s Path=%s TESSound=%p",
                     evt.soundFlags, GetSoundCategory(evt.soundFlags),
                     filePath, evt.sourceSound);

            for (Script* callback : OnSoundPlayedHandler::g_callbacks)
            {
                if (g_osphScript && callback)
                {
                    g_osphScript->CallFunctionAlt(
                        callback,
                        nullptr,
                        3,
                        filePath,
                        evt.soundFlags,
                        evt.sourceSound
                    );
                }
            }
        }
    }

    // Check tracked voice sounds for completion
    if (!soundsToCheck.empty() && !OnSoundPlayedHandler::g_completionCallbacks.empty())
    {
        std::vector<UInt32> completedIDs;

        for (const auto& tracked : soundsToCheck)
        {
            //build a temp BSSoundHandle to check
            BSSoundHandle tempHandle;
            tempHandle.uiSoundID = tracked.soundID;
            tempHandle.bAssumeSuccess = 0;
            tempHandle.uiState = 0;

            bool stillPlaying = BSSoundHandle_IsPlaying(&tempHandle);

            if (!stillPlaying)
            {
                OSPH_Log("Voice completed: ID=%08X Path=%s", tracked.soundID, tracked.filePath);
                completedIDs.push_back(tracked.soundID);

                //fire completion callbacks
                for (Script* callback : OnSoundPlayedHandler::g_completionCallbacks)
                {
                    if (g_osphScript && callback)
                    {
                        g_osphScript->CallFunctionAlt(
                            callback,
                            nullptr,
                            3,
                            tracked.filePath,
                            tracked.soundFlags,
                            tracked.sourceSound
                        );
                    }
                }
            }
        }

        //remove completed sounds from tracking
        if (!completedIDs.empty())
        {
            EnterCriticalSection(&OnSoundPlayedHandler::g_queueLock);
            for (UInt32 id : completedIDs)
            {
                auto& vec = OnSoundPlayedHandler::g_trackedSounds;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [id](const TrackedVoiceSound& s) { return s.soundID == id; }), vec.end());
            }
            LeaveCriticalSection(&OnSoundPlayedHandler::g_queueLock);
        }
    }
}

static void InitHook()
{
    if (OnSoundPlayedHandler::g_hookInstalled) return;

    // Initialize critical section for thread-safe queue
    if (!OnSoundPlayedHandler::g_lockInitialized)
    {
        InitializeCriticalSection(&OnSoundPlayedHandler::g_queueLock);
        OnSoundPlayedHandler::g_lockInitialized = true;
    }

    OSPH_Log("Installing BSAudioManager::GetSoundHandleByFilePath hook at 0x%X", kAddr_GetSoundHandleByFilePath);

    // Allocate executable memory for trampoline
    s_trampoline = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_trampoline)
    {
        OSPH_Log("ERROR: Failed to allocate trampoline memory");
        return;
    }

    // Prologue at 0xAE5A50:
    //   55       push ebp        (1 byte)
    //   8B EC    mov ebp, esp    (2 bytes)
    //   6A FF    push -1         (2 bytes)
    // Total: 5 bytes - exactly what we need for JMP rel32

    // Copy original bytes to trampoline (first 5 bytes of the function)
    memcpy(s_trampoline, (void*)kAddr_GetSoundHandleByFilePath, 5);

    // Add JMP back to original function + 5
    s_trampoline[5] = 0xE9; // JMP rel32
    *(UInt32*)(s_trampoline + 6) = (kAddr_GetSoundHandleByFilePath + 5) - (UInt32)(s_trampoline + 10);

    OriginalGetSoundHandle = (GetSoundHandleByFilePath_t)s_trampoline;

    // Patch the original function to jump to our hook (exactly 5 bytes)
    DWORD oldProtect;
    VirtualProtect((void*)kAddr_GetSoundHandleByFilePath, 5, PAGE_EXECUTE_READWRITE, &oldProtect);

    UInt8* patchAddr = (UInt8*)kAddr_GetSoundHandleByFilePath;
    patchAddr[0] = 0xE9; // JMP rel32
    *(UInt32*)(patchAddr + 1) = (UInt32)HookedGetSoundHandle - (kAddr_GetSoundHandleByFilePath + 5);

    VirtualProtect((void*)kAddr_GetSoundHandleByFilePath, 5, oldProtect, &oldProtect);

    OnSoundPlayedHandler::g_hookInstalled = true;
    OSPH_Log("Hook installed successfully. Trampoline at %p", s_trampoline);
}

static bool AddCallback(Script* callback)
{
    OSPH_Log("AddCallback called with callback=0x%08X", callback);
    if (!callback) return false;

    // Check for duplicates
    for (Script* s : OnSoundPlayedHandler::g_callbacks)
    {
        if (s == callback)
        {
            OSPH_Log("Callback already registered");
            return false;
        }
    }

    OnSoundPlayedHandler::g_callbacks.push_back(callback);
    OSPH_Log("Callback added, total: %d", (int)OnSoundPlayedHandler::g_callbacks.size());

    // Install hook on first callback
    if (!OnSoundPlayedHandler::g_hookInstalled)
    {
        InitHook();
    }

    return true;
}

static bool AddCompletionCallback(Script* callback)
{
    OSPH_Log("AddCompletionCallback called with callback=0x%08X", callback);
    if (!callback) return false;

    for (Script* s : OnSoundPlayedHandler::g_completionCallbacks)
    {
        if (s == callback)
        {
            OSPH_Log("Completion callback already registered");
            return false;
        }
    }

    OnSoundPlayedHandler::g_completionCallbacks.push_back(callback);
    OSPH_Log("Completion callback added, total: %d", (int)OnSoundPlayedHandler::g_completionCallbacks.size());

    // Install hook if not already
    if (!OnSoundPlayedHandler::g_hookInstalled)
    {
        InitHook();
    }

    return true;
}

static bool RemoveCompletionCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnSoundPlayedHandler::g_completionCallbacks.begin();
         it != OnSoundPlayedHandler::g_completionCallbacks.end(); ++it)
    {
        if (*it == callback)
        {
            OnSoundPlayedHandler::g_completionCallbacks.erase(it);
            OSPH_Log("Removed completion callback: 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static bool RemoveCallback(Script* callback)
{
    if (!callback) return false;

    for (auto it = OnSoundPlayedHandler::g_callbacks.begin();
         it != OnSoundPlayedHandler::g_callbacks.end(); ++it)
    {
        if (*it == callback)
        {
            OnSoundPlayedHandler::g_callbacks.erase(it);
            OSPH_Log("Removed callback: 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static ParamInfo kParams_SoundPlayedHandler[2] = {
    {"callback",   kParamType_AnyForm, 0},
    {"addRemove",  kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetOnSoundPlayedEventHandler,
    "Registers/unregisters a callback for sound played events. Callback receives: filePath, soundFlags, TESSound",
    0, 2, kParams_SoundPlayedHandler);

bool Cmd_SetOnSoundPlayedEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OSPH_Log("SetOnSoundPlayedEventHandler called");

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove))
    {
        OSPH_Log("Failed to extract args");
        return true;
    }

    OSPH_Log("Extracted args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm)
    {
        OSPH_Log("Callback is null");
        return true;
    }

    // Check if it's a script
    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script)
    {
        OSPH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove)
    {
        if (AddCallback(callback))
        {
            *result = 1;
            OSPH_Log("Callback added successfully");
        }
    }
    else
    {
        if (RemoveCallback(callback))
        {
            *result = 1;
            OSPH_Log("Callback removed successfully");
        }
    }

    return true;
}

bool OSPH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    // Open log file
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnSoundPlayedHandler.log");
    g_osphLogFile = fopen(logPath, "w");

    OSPH_Log("OnSoundPlayedHandler module initializing...");

    g_osphPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_osphScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_osphScript)
    {
        OSPH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_osphScript->ExtractArgsEx;
    OSPH_Log("Script interface at 0x%08X", g_osphScript);

    // Register commands at opcode 0x3B19
    nvse->SetOpcodeBase(0x4016);
    nvse->RegisterCommand(&kCommandInfo_SetOnSoundPlayedEventHandler);
    g_osphOpcode = 0x4016;

    OSPH_Log("SoundPlayed: name=%s execute=0x%08X", kCommandInfo_SetOnSoundPlayedEventHandler.longName, (UInt32)kCommandInfo_SetOnSoundPlayedEventHandler.execute);

    //install hook at init time to avoid race conditions
    InitHook();

    OSPH_Log("OnSoundPlayedHandler module initialized successfully");

    return true;
}

unsigned int OSPH_GetOpcode()
{
    return g_osphOpcode;
}
