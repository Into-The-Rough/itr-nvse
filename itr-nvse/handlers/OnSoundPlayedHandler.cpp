//hooks BSAudioManager::GetSoundHandleByFilePath at 0xAE5A50 to catch ALL sounds
//uses a queue to dispatch events on the main thread (audio may run on separate thread)

#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <Windows.h>

#include "OnSoundPlayedHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

class TESSound;

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
    static BSAudioManager* Get() { return (BSAudioManager*)0x11F6EF0; }
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
static UInt32 g_osphCompletionOpcode = 0;

class ScopedLock {
    CRITICAL_SECTION* cs;
public:
    ScopedLock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
    ~ScopedLock() { LeaveCriticalSection(cs); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};

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
static Detours::JumpDetour s_detour;

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

    //prevent unbounded queue growth
    constexpr size_t kMaxQueueSize = 256;
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
        if (OnSoundPlayedHandler::g_pendingEvents.size() >= kMaxQueueSize)
            return;
    }

    QueuedSoundEvent evt;

    // Copy file path
    if (filePath && filePath[0])
    {
        strncpy_s(evt.filePath, sizeof(evt.filePath), filePath, _TRUNCATE);
    }
    else
    {
        evt.filePath[0] = '\0';
    }

    evt.soundFlags = flags;
    evt.sourceSound = sourceSound;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
        OnSoundPlayedHandler::g_pendingEvents.push_back(evt);
    }
}

// Queue a voice sound for completion tracking
static void QueueVoiceTracking(UInt32 soundID, const char* filePath, UInt32 flags, TESSound* sourceSound)
{
    if (!OnSoundPlayedHandler::g_lockInitialized) return;
    if (soundID == 0 || soundID == 0xFFFFFFFF) return;

    //prevent unbounded tracking list growth
    constexpr size_t kMaxTrackedSounds = 64;
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
        if (OnSoundPlayedHandler::g_trackedSounds.size() >= kMaxTrackedSounds)
            return;
    }

    TrackedVoiceSound tracked;
    tracked.soundID = soundID;
    tracked.soundFlags = flags;
    tracked.sourceSound = sourceSound;

    if (filePath && filePath[0])
    {
        strncpy_s(tracked.filePath, sizeof(tracked.filePath), filePath, _TRUNCATE);
    }
    else
    {
        tracked.filePath[0] = '\0';
    }

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
        OnSoundPlayedHandler::g_trackedSounds.push_back(tracked);
    }

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
    bool hasPlayCallbacks = false;
    bool hasCompletionCallbacks = false;
    if (OnSoundPlayedHandler::g_lockInitialized)
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
        hasPlayCallbacks = !OnSoundPlayedHandler::g_callbacks.empty();
        hasCompletionCallbacks = !OnSoundPlayedHandler::g_completionCallbacks.empty();
    }

    // Queue event if we have callbacks
    if (hasPlayCallbacks && apName && apName[0])
    {
        QueueSoundEvent(apName, aeAudioFlags, apSound);
    }

    // Call original
    BSSoundHandle* result = s_detour.GetTrampoline<GetSoundHandleByFilePath_t>()(mgr, arData, apName, aeAudioFlags, apSound);

    // Track voice sounds for completion detection
    if (hasCompletionCallbacks &&
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
    std::vector<Script*> playCallbacks;
    std::vector<Script*> completionCallbacks;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
        eventsToProcess.swap(OnSoundPlayedHandler::g_pendingEvents);
        soundsToCheck = OnSoundPlayedHandler::g_trackedSounds; //copy for checking
        playCallbacks = OnSoundPlayedHandler::g_callbacks;
        completionCallbacks = OnSoundPlayedHandler::g_completionCallbacks;
    }

    // Process sound started events
    if (!eventsToProcess.empty() && !playCallbacks.empty())
    {
        for (const auto& evt : eventsToProcess)
        {
            const char* filePath = evt.filePath[0] ? evt.filePath : "";

            OSPH_Log("Dispatching: Flags=0x%X Category=%s Path=%s TESSound=%p",
                     evt.soundFlags, GetSoundCategory(evt.soundFlags),
                     filePath, evt.sourceSound);

            for (Script* callback : playCallbacks)
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
    if (!soundsToCheck.empty() && !completionCallbacks.empty())
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
                for (Script* callback : completionCallbacks)
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
            ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
            for (UInt32 id : completedIDs)
            {
                auto& vec = OnSoundPlayedHandler::g_trackedSounds;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [id](const TrackedVoiceSound& s) { return s.soundID == id; }), vec.end());
            }
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

    //prologue: push ebp (1) + mov ebp,esp (2) + push -1 (2) = 5 bytes
    if (!s_detour.WriteRelJump(kAddr_GetSoundHandleByFilePath, HookedGetSoundHandle, 5))
    {
        OSPH_Log("ERROR: Failed to install hook");
        return;
    }

    OnSoundPlayedHandler::g_hookInstalled = true;
    OSPH_Log("Hook installed successfully");
}

static bool AddCallback(Script* callback)
{
    OSPH_Log("AddCallback called with callback=0x%08X", callback);
    if (!callback) return false;

    if (!OnSoundPlayedHandler::g_hookInstalled)
    {
        InitHook();
    }

    if (!OnSoundPlayedHandler::g_lockInitialized) return false;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);

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
    }

    return true;
}

static bool AddCompletionCallback(Script* callback)
{
    OSPH_Log("AddCompletionCallback called with callback=0x%08X", callback);
    if (!callback) return false;

    if (!OnSoundPlayedHandler::g_hookInstalled)
    {
        InitHook();
    }

    if (!OnSoundPlayedHandler::g_lockInitialized) return false;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
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
    }

    return true;
}

static bool RemoveCompletionCallback(Script* callback)
{
    if (!callback) return false;
    if (!OnSoundPlayedHandler::g_lockInitialized) return false;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
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
    }
    return false;
}

static bool RemoveCallback(Script* callback)
{
    if (!callback) return false;
    if (!OnSoundPlayedHandler::g_lockInitialized) return false;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
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
DEFINE_COMMAND_PLUGIN(SetOnSoundCompletedEventHandler,
    "Registers/unregisters a callback for voice completion events. Callback receives: filePath, soundFlags, TESSound",
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

bool Cmd_SetOnSoundCompletedEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OSPH_Log("SetOnSoundCompletedEventHandler called");

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

    OSPH_Log("Extracted completion args: callback=0x%08X, add=%d", callbackForm, addRemove);

    if (!callbackForm)
    {
        OSPH_Log("Completion callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script)
    {
        OSPH_Log("Completion callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove)
    {
        if (AddCompletionCallback(callback))
        {
            *result = 1;
            OSPH_Log("Completion callback added successfully");
        }
    }
    else
    {
        if (RemoveCompletionCallback(callback))
        {
            *result = 1;
            OSPH_Log("Completion callback removed successfully");
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
    //g_osphLogFile = fopen(logPath, "w"); //disabled for release

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

    // Register commands
    nvse->SetOpcodeBase(0x4016);
    nvse->RegisterCommand(&kCommandInfo_SetOnSoundPlayedEventHandler);
    g_osphOpcode = 0x4016;

    nvse->SetOpcodeBase(0x403C);
    nvse->RegisterCommand(&kCommandInfo_SetOnSoundCompletedEventHandler);
    g_osphCompletionOpcode = 0x403C;

    OSPH_Log("SoundPlayed: name=%s execute=0x%08X", kCommandInfo_SetOnSoundPlayedEventHandler.longName, (UInt32)kCommandInfo_SetOnSoundPlayedEventHandler.execute);
    OSPH_Log("SoundCompleted: name=%s execute=0x%08X", kCommandInfo_SetOnSoundCompletedEventHandler.longName, (UInt32)kCommandInfo_SetOnSoundCompletedEventHandler.execute);

    //install hook at init time to avoid race conditions
    InitHook();

    OSPH_Log("OnSoundPlayedHandler module initialized successfully");

    return true;
}

unsigned int OSPH_GetOpcode()
{
    return g_osphOpcode;
}

unsigned int OSPH_GetCompletionOpcode()
{
    return g_osphCompletionOpcode;
}

void OSPH_ClearCallbacks()
{
    if (!OnSoundPlayedHandler::g_lockInitialized)
    {
        OnSoundPlayedHandler::g_callbacks.clear();
        OnSoundPlayedHandler::g_completionCallbacks.clear();
        return;
    }

    ScopedLock lock(&OnSoundPlayedHandler::g_queueLock);
    OnSoundPlayedHandler::g_callbacks.clear();
    OnSoundPlayedHandler::g_completionCallbacks.clear();
    OnSoundPlayedHandler::g_pendingEvents.clear();
    OnSoundPlayedHandler::g_trackedSounds.clear();
    OSPH_Log("Callbacks cleared on game load");
}
