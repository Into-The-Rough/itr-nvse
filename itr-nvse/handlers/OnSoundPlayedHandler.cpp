//hooks BSAudioManager::GetSoundHandleByFilePath at 0xAE5A50 to catch ALL sounds
//uses a queue to dispatch events on the main thread (audio may run on separate thread)

#include <vector>
#include <algorithm>
#include <cstring>
#include <Windows.h>

#include "OnSoundPlayedHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"

class TESSound;

struct BSSoundHandle
{
    UInt32 uiSoundID;
    UInt8 bAssumeSuccess;
    UInt8 pad[3];
    UInt32 uiState;
};

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

struct BSAudioManager
{
    static BSAudioManager* Get() { return (BSAudioManager*)0x11F6EF0; }
};

struct QueuedSoundEvent
{
    char filePath[260];
    UInt32 soundFlags;
    UInt32 soundFormID;
};

struct TrackedVoiceSound
{
    UInt32 soundID;
    char filePath[260];
    UInt32 soundFlags;
    UInt32 soundFormID;
    BSSoundHandle handleState;
    bool hasEverPlayed;
    UInt32 pollCount;
};

#include "internal/EngineFunctions.h"

static PluginHandle g_osphPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_osphScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_osphOpcode = 0;
static UInt32 g_osphCompletionOpcode = 0;

namespace OnSoundPlayedHandler {
    std::vector<Script*> g_callbacks;
    std::vector<Script*> g_completionCallbacks;
    bool g_hookInstalled = false;

    std::vector<QueuedSoundEvent> g_pendingEvents;
    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;

    std::vector<TrackedVoiceSound> g_trackedSounds;
    DWORD g_mainThreadId = 0;
}

static void EnsureStateLockInitialized()
{
    if (OnSoundPlayedHandler::g_stateLockInit == 2) return;

    if (InterlockedCompareExchange(&OnSoundPlayedHandler::g_stateLockInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&OnSoundPlayedHandler::g_stateLock);
        InterlockedExchange(&OnSoundPlayedHandler::g_stateLockInit, 2);
        return;
    }

    while (OnSoundPlayedHandler::g_stateLockInit != 2)
        Sleep(0);
}

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
}

static TESForm* LookupFormByID(UInt32 refID)
{
    if (!refID) return nullptr;
    struct Entry { Entry* next; UInt32 key; TESForm* form; };
    UInt8* map = *(UInt8**)0x11C54C0;
    if (!map) return nullptr;
    UInt32 numBuckets = *(UInt32*)(map + 4);
    Entry** buckets = *(Entry***)(map + 8);
    if (!buckets || !numBuckets) return nullptr;
    for (Entry* e = buckets[refID % numBuckets]; e; e = e->next)
        if (e->key == refID) return e->form;
    return nullptr;
}

typedef BSSoundHandle* (__thiscall* GetSoundHandleByFilePath_t)(
    BSAudioManager* mgr,
    BSSoundHandle* arData,
    const char* apName,
    UInt32 aeAudioFlags,
    TESSound* apSound
);
static Detours::JumpDetour s_detour;

//queue a sound event (may be called from audio thread - must be thread-safe)
static void QueueSoundEvent(const char* filePath, UInt32 flags, UInt32 soundFormID)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;

    //prevent unbounded queue growth
    constexpr size_t kMaxQueueSize = 256;
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        if (OnSoundPlayedHandler::g_pendingEvents.size() >= kMaxQueueSize) {
            return;
        }
    }

    QueuedSoundEvent evt;

    if (filePath && filePath[0])
    {
        strncpy_s(evt.filePath, sizeof(evt.filePath), filePath, _TRUNCATE);
    }
    else
    {
        evt.filePath[0] = '\0';
    }

    evt.soundFlags = flags;
    evt.soundFormID = soundFormID;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        OnSoundPlayedHandler::g_pendingEvents.push_back(evt);
    }
}

static void QueueVoiceTracking(UInt32 soundID, const char* filePath, UInt32 flags, UInt32 soundFormID, const BSSoundHandle* handle)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    if (soundID == 0 || soundID == 0xFFFFFFFF) return;

    //prevent unbounded tracking list growth
    constexpr size_t kMaxTrackedSounds = 64;
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        if (OnSoundPlayedHandler::g_trackedSounds.size() >= kMaxTrackedSounds) {
            return;
        }
    }

    TrackedVoiceSound tracked;
    tracked.soundID = soundID;
    tracked.soundFlags = flags;
    tracked.soundFormID = soundFormID;
    tracked.hasEverPlayed = false;
    tracked.pollCount = 0;

    if (handle)
    {
        tracked.handleState = *handle;
    }
    else
    {
        tracked.handleState.uiSoundID = soundID;
        tracked.handleState.bAssumeSuccess = 1;
        tracked.handleState.uiState = 0;
        tracked.handleState.pad[0] = tracked.handleState.pad[1] = tracked.handleState.pad[2] = 0;
    }

    if (filePath && filePath[0])
    {
        strncpy_s(tracked.filePath, sizeof(tracked.filePath), filePath, _TRUNCATE);
    }
    else
    {
        tracked.filePath[0] = '\0';
    }

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        OnSoundPlayedHandler::g_trackedSounds.push_back(tracked);
    }

}

static BSSoundHandle* __fastcall HookedGetSoundHandle(
    BSAudioManager* mgr, void* edx,
    BSSoundHandle* arData,
    const char* apName,
    UInt32 aeAudioFlags,
    TESSound* apSound)
{
    bool hasPlayCallbacks = false;
    bool hasCompletionCallbacks = false;
    UInt32 soundFormID = ReadRefID(apSound);
    if (OnSoundPlayedHandler::g_stateLockInit == 2)
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        hasPlayCallbacks = !OnSoundPlayedHandler::g_callbacks.empty();
        hasCompletionCallbacks = !OnSoundPlayedHandler::g_completionCallbacks.empty();
    }

    if (hasPlayCallbacks && apName && apName[0])
    {
        QueueSoundEvent(apName, aeAudioFlags, soundFormID);
    }

    BSSoundHandle* result = s_detour.GetTrampoline<GetSoundHandleByFilePath_t>()(mgr, arData, apName, aeAudioFlags, apSound);

    if (hasCompletionCallbacks &&
        (aeAudioFlags & kSoundFlag_IsVoice) && apName && apName[0])
    {
        if (result && result->uiSoundID != 0 && result->uiSoundID != 0xFFFFFFFF)
        {
            QueueVoiceTracking(result->uiSoundID, apName, aeAudioFlags, soundFormID, result);
        }
    }

    return result;
}

void OSPH_Update()
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    if (!g_osphScript) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnSoundPlayedHandler::g_mainThreadId)
        OnSoundPlayedHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnSoundPlayedHandler::g_mainThreadId)
        return;

    std::vector<QueuedSoundEvent> eventsToProcess;
    std::vector<TrackedVoiceSound> soundsToCheck;
    std::vector<Script*> playCallbacks;
    std::vector<Script*> completionCallbacks;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        eventsToProcess.swap(OnSoundPlayedHandler::g_pendingEvents);
        soundsToCheck = OnSoundPlayedHandler::g_trackedSounds; //copy for checking
        playCallbacks = OnSoundPlayedHandler::g_callbacks;
        completionCallbacks = OnSoundPlayedHandler::g_completionCallbacks;
    }

    if (!eventsToProcess.empty() && !playCallbacks.empty())
    {
        for (const auto& evt : eventsToProcess)
        {
            const char* filePath = evt.filePath[0] ? evt.filePath : "";
            TESForm* sourceSound = LookupFormByID(evt.soundFormID);

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
                        sourceSound
                    );
                }
            }
        }
    }

    if (!soundsToCheck.empty() && !completionCallbacks.empty())
    {
        std::vector<TrackedVoiceSound> completedSounds;
        std::vector<TrackedVoiceSound> updatedSounds;
        completedSounds.reserve(soundsToCheck.size());
        updatedSounds.reserve(soundsToCheck.size());

        for (auto& tracked : soundsToCheck)
        {
            ++tracked.pollCount;
            bool stillPlaying = Engine::BSSoundHandle_IsPlaying(&tracked.handleState);
            if (stillPlaying)
                tracked.hasEverPlayed = true;

            if (!stillPlaying && tracked.hasEverPlayed)
            {
                completedSounds.push_back(tracked);
                TESForm* sourceSound = LookupFormByID(tracked.soundFormID);

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
                            sourceSound
                        );
                    }
                }
            }
            else
            {
                constexpr UInt32 kMaxPollsWithoutPlayback = 180;
                if (!tracked.hasEverPlayed && tracked.pollCount >= kMaxPollsWithoutPlayback)
                {
                    completedSounds.push_back(tracked);
                }
                else
                {
                    updatedSounds.push_back(tracked);
                }
            }
        }

        auto isSameTracked = [](const TrackedVoiceSound& a, const TrackedVoiceSound& b) -> bool
        {
            return a.soundID == b.soundID &&
                   a.soundFormID == b.soundFormID &&
                   std::strcmp(a.filePath, b.filePath) == 0;
        };

        {
            ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
            auto& liveTracked = OnSoundPlayedHandler::g_trackedSounds;

            for (const auto& updated : updatedSounds)
            {
                for (auto& live : liveTracked)
                {
                    if (isSameTracked(live, updated))
                    {
                        live.handleState = updated.handleState;
                        live.hasEverPlayed = updated.hasEverPlayed;
                        live.pollCount = updated.pollCount;
                        break;
                    }
                }
            }

            if (!completedSounds.empty())
            {
                liveTracked.erase(
                    std::remove_if(liveTracked.begin(), liveTracked.end(),
                        [&](const TrackedVoiceSound& live)
                        {
                            for (const auto& completed : completedSounds)
                            {
                                if (isSameTracked(live, completed))
                                    return true;
                            }
                            return false;
                        }),
                    liveTracked.end());
            }
        }
    }
}

static void InitHook()
{
    if (OnSoundPlayedHandler::g_hookInstalled) return;

    EnsureStateLockInitialized();

    //prologue: push ebp (1) + mov ebp,esp (2) + push -1 (2) = 5 bytes
    if (!s_detour.WriteRelJump(0xAE5A50, HookedGetSoundHandle, 5))
    {
        return;
    }

    OnSoundPlayedHandler::g_hookInstalled = true;
}

static bool AddCallback(Script* callback)
{
    if (!callback) return false;

    EnsureStateLockInitialized();
    if (!OnSoundPlayedHandler::g_hookInstalled)
    {
        InitHook();
    }

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);

        for (Script* s : OnSoundPlayedHandler::g_callbacks)
        {
            if (s == callback)
            {
                return false;
            }
        }

        OnSoundPlayedHandler::g_callbacks.push_back(callback);
    }

    return true;
}

static bool AddCompletionCallback(Script* callback)
{
    if (!callback) return false;

    EnsureStateLockInitialized();
    if (!OnSoundPlayedHandler::g_hookInstalled)
    {
        InitHook();
    }

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        for (Script* s : OnSoundPlayedHandler::g_completionCallbacks)
        {
            if (s == callback)
            {
                return false;
            }
        }

        OnSoundPlayedHandler::g_completionCallbacks.push_back(callback);
    }

    return true;
}

static bool RemoveCompletionCallback(Script* callback)
{
    if (!callback) return false;
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return false;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        for (auto it = OnSoundPlayedHandler::g_completionCallbacks.begin();
             it != OnSoundPlayedHandler::g_completionCallbacks.end(); ++it)
        {
            if (*it == callback)
            {
                OnSoundPlayedHandler::g_completionCallbacks.erase(it);
                return true;
            }
        }
    }
    return false;
}

static bool RemoveCallback(Script* callback)
{
    if (!callback) return false;
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return false;

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        for (auto it = OnSoundPlayedHandler::g_callbacks.begin();
             it != OnSoundPlayedHandler::g_callbacks.end(); ++it)
        {
            if (*it == callback)
            {
                OnSoundPlayedHandler::g_callbacks.erase(it);
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
        return true;
    }

    if (!callbackForm)
    {
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script)
    {
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove)
    {
        if (AddCallback(callback))
        {
            *result = 1;
        }
    }
    else
    {
        if (RemoveCallback(callback))
        {
            *result = 1;
        }
    }

    return true;
}

bool Cmd_SetOnSoundCompletedEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;

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
        return true;
    }

    if (!callbackForm)
    {
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script)
    {
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove)
    {
        if (AddCompletionCallback(callback))
        {
            *result = 1;
        }
    }
    else
    {
        if (RemoveCompletionCallback(callback))
        {
            *result = 1;
        }
    }

    return true;
}

bool OSPH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_osphPluginHandle = nvse->GetPluginHandle();

    g_osphScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_osphScript)
    {
        return false;
    }

    g_ExtractArgsEx = g_osphScript->ExtractArgsEx;

    EnsureStateLockInitialized();
    OnSoundPlayedHandler::g_mainThreadId = GetCurrentThreadId();

    nvse->SetOpcodeBase(0x4016);
    nvse->RegisterCommand(&kCommandInfo_SetOnSoundPlayedEventHandler);
    g_osphOpcode = 0x4016;

    nvse->SetOpcodeBase(0x403C);
    nvse->RegisterCommand(&kCommandInfo_SetOnSoundCompletedEventHandler);
    g_osphCompletionOpcode = 0x403C;

    //install hook at init time to avoid race conditions
    InitHook();

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
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;

    ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
    OnSoundPlayedHandler::g_callbacks.clear();
    OnSoundPlayedHandler::g_completionCallbacks.clear();
    OnSoundPlayedHandler::g_pendingEvents.clear();
    OnSoundPlayedHandler::g_trackedSounds.clear();
}
