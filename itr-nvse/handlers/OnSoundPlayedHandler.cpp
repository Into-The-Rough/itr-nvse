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
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"

class TESSound;

struct BSSoundHandle
{
    UInt32 uiSoundID;
    UInt8 bAssumeSuccess;
    UInt8 pad[3];
    UInt32 uiState;
};

struct BSAudioManager
{
    static BSAudioManager* Get() { return (BSAudioManager*)g_audioManager; }
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

namespace OnSoundPlayedHandler {
    bool g_hookInstalled = false;
    std::vector<QueuedSoundEvent> g_pendingEvents;
    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;
    std::vector<TrackedVoiceSound> g_trackedSounds;
    DWORD g_mainThreadId = 0;
}

static constexpr const char* kSoundPlayedEvent = "ITR:OnSoundPlayed";
static constexpr const char* kSoundCompletedEvent = "ITR:OnSoundCompleted";
static constexpr int kProbePriority = -9999;
static constexpr UInt32 kListenerProbeIntervalFrames = 30;

static PluginHandle s_pluginHandle = kPluginHandle_Invalid;
static bool s_probeHandlersInstalled = false;
static volatile bool s_hasPlayedListeners = true;
static volatile bool s_hasCompletedListeners = true;
static UInt32 s_listenerProbeFrame = kListenerProbeIntervalFrames;

static void SoundPlayedProbeHandler(TESObjectREFR*, void*) {}
static void SoundCompletedProbeHandler(TESObjectREFR*, void*) {}

static bool InstallProbeHandler(const char* eventName, NVSEEventManagerInterface::NativeEventHandler handler,
    const char* handlerName)
{
    if (!g_eventManagerInterface ||
        !g_eventManagerInterface->SetNativeEventHandlerWithPriority ||
        !g_eventManagerInterface->RemoveNativeEventHandlerWithPriority ||
        s_pluginHandle == kPluginHandle_Invalid)
    {
        return false;
    }

    g_eventManagerInterface->RemoveNativeEventHandlerWithPriority(eventName, handler, kProbePriority);
    return g_eventManagerInterface->SetNativeEventHandlerWithPriority(
        eventName, handler, s_pluginHandle, handlerName, kProbePriority);
}

static bool HasExternalHandlers(const char* eventName, NVSEEventManagerInterface::NativeEventHandler probeHandler) {
    if (!s_probeHandlersInstalled || !g_eventManagerInterface || !g_eventManagerInterface->IsEventHandlerFirst)
        return true;

    return !g_eventManagerInterface->IsEventHandlerFirst(
        eventName, probeHandler, kProbePriority,
        nullptr, 0, nullptr, 0, nullptr, 0);
}

static void RefreshListenerState(bool force) {
    if (!s_probeHandlersInstalled)
        OnSoundPlayedHandler::InstallListenerProbes();

    if (!s_probeHandlersInstalled) {
        s_hasPlayedListeners = true;
        s_hasCompletedListeners = true;
        return;
    }

    if (!force && s_listenerProbeFrame++ < kListenerProbeIntervalFrames)
        return;

    s_listenerProbeFrame = 0;
    s_hasPlayedListeners = HasExternalHandlers(kSoundPlayedEvent, SoundPlayedProbeHandler);
    s_hasCompletedListeners = HasExternalHandlers(kSoundCompletedEvent, SoundCompletedProbeHandler);
}

static void EnsureStateLockInitialized()
{
    InitCriticalSectionOnce(&OnSoundPlayedHandler::g_stateLockInit, &OnSoundPlayedHandler::g_stateLock);
}

static UInt32 ReadRefID(const TESForm* form)
{
    return form ? form->refID : 0;
}

typedef BSSoundHandle* (__thiscall* GetSoundHandleByFilePath_t)(
    BSAudioManager* mgr, BSSoundHandle* arData,
    const char* apName, UInt32 aeAudioFlags, TESSound* apSound
);
static Detours::JumpDetour s_detour;

static void QueueSoundEvent(const char* filePath, UInt32 flags, UInt32 soundFormID)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;

    constexpr size_t kMaxQueueSize = 256;

    QueuedSoundEvent evt;
    if (filePath && filePath[0])
        strncpy_s(evt.filePath, sizeof(evt.filePath), filePath, _TRUNCATE);
    else
        evt.filePath[0] = '\0';
    evt.soundFlags = flags;
    evt.soundFormID = soundFormID;

    ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
    if (OnSoundPlayedHandler::g_pendingEvents.size() >= kMaxQueueSize) return;
    OnSoundPlayedHandler::g_pendingEvents.push_back(evt);
}

static void QueueVoiceTracking(UInt32 soundID, const char* filePath, UInt32 flags, UInt32 soundFormID, const BSSoundHandle* handle)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    if (soundID == 0 || soundID == 0xFFFFFFFF) return;

    constexpr size_t kMaxTrackedSounds = 64;

    TrackedVoiceSound tracked;
    tracked.soundID = soundID;
    tracked.soundFlags = flags;
    tracked.soundFormID = soundFormID;
    tracked.hasEverPlayed = false;
    tracked.pollCount = 0;

    if (handle)
        tracked.handleState = *handle;
    else {
        tracked.handleState.uiSoundID = soundID;
        tracked.handleState.bAssumeSuccess = 1;
        tracked.handleState.uiState = 0;
        tracked.handleState.pad[0] = tracked.handleState.pad[1] = tracked.handleState.pad[2] = 0;
    }

    if (filePath && filePath[0])
        strncpy_s(tracked.filePath, sizeof(tracked.filePath), filePath, _TRUNCATE);
    else
        tracked.filePath[0] = '\0';

    ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
    if (OnSoundPlayedHandler::g_trackedSounds.size() >= kMaxTrackedSounds) return;
    OnSoundPlayedHandler::g_trackedSounds.push_back(tracked);
}

enum { kSoundFlag_IsVoice = 0x4 };

static BSSoundHandle* __fastcall HookedGetSoundHandle(
    BSAudioManager* mgr, void* edx,
    BSSoundHandle* arData, const char* apName,
    UInt32 aeAudioFlags, TESSound* apSound)
{
    bool hasPlayedListeners = s_hasPlayedListeners;
    bool hasCompletedListeners = s_hasCompletedListeners;
    UInt32 soundFormID = ReadRefID(apSound);
    bool hasEventManager = g_eventManagerInterface != nullptr;

    if (hasEventManager && hasPlayedListeners && apName && apName[0])
        QueueSoundEvent(apName, aeAudioFlags, soundFormID);

    BSSoundHandle* result = s_detour.GetTrampoline<GetSoundHandleByFilePath_t>()(mgr, arData, apName, aeAudioFlags, apSound);

    if (hasEventManager && hasCompletedListeners && (aeAudioFlags & kSoundFlag_IsVoice) && apName && apName[0]) {
        if (result && result->uiSoundID != 0 && result->uiSoundID != 0xFFFFFFFF)
            QueueVoiceTracking(result->uiSoundID, apName, aeAudioFlags, soundFormID, result);
    }

    return result;
}

namespace OnSoundPlayedHandler {
void InstallListenerProbes()
{
    bool playedProbe = InstallProbeHandler(kSoundPlayedEvent, SoundPlayedProbeHandler, "ITR_OnSoundPlayedProbe");
    bool completedProbe = InstallProbeHandler(kSoundCompletedEvent, SoundCompletedProbeHandler, "ITR_OnSoundCompletedProbe");

    s_probeHandlersInstalled = playedProbe && completedProbe;
    s_listenerProbeFrame = kListenerProbeIntervalFrames;
    s_hasPlayedListeners = true;
    s_hasCompletedListeners = true;
}

void Update()
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    if (!g_eventManagerInterface) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnSoundPlayedHandler::g_mainThreadId)
        OnSoundPlayedHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnSoundPlayedHandler::g_mainThreadId)
        return;

    RefreshListenerState(false);

    std::vector<QueuedSoundEvent> eventsToProcess;
    std::vector<TrackedVoiceSound> soundsToCheck;
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        eventsToProcess.swap(OnSoundPlayedHandler::g_pendingEvents);
        soundsToCheck = OnSoundPlayedHandler::g_trackedSounds;
    }

    for (const auto& evt : eventsToProcess)
    {
        const char* filePath = evt.filePath[0] ? evt.filePath : "";
        TESForm* sourceSound = evt.soundFormID ? (TESForm*)Engine::LookupFormByID(evt.soundFormID) : nullptr;
        if (!sourceSound) continue;

        g_eventManagerInterface->DispatchEvent(kSoundPlayedEvent, nullptr,
            filePath, (int)evt.soundFlags, (TESObjectREFR*)sourceSound);
    }

    if (!soundsToCheck.empty())
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
                TESForm* sourceSound = tracked.soundFormID ? (TESForm*)Engine::LookupFormByID(tracked.soundFormID) : nullptr;

                if (sourceSound)
                    g_eventManagerInterface->DispatchEvent(kSoundCompletedEvent, nullptr,
                        tracked.filePath, (int)tracked.soundFlags, (TESObjectREFR*)sourceSound);
            }
            else
            {
                constexpr UInt32 kMaxPollsWithoutPlayback = 180;
                if (!tracked.hasEverPlayed && tracked.pollCount >= kMaxPollsWithoutPlayback)
                    completedSounds.push_back(tracked);
                else
                    updatedSounds.push_back(tracked);
            }
        }

        auto isSameTracked = [](const TrackedVoiceSound& a, const TrackedVoiceSound& b) -> bool {
            return a.soundID == b.soundID && a.soundFormID == b.soundFormID &&
                   std::strcmp(a.filePath, b.filePath) == 0;
        };

        {
            ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
            auto& liveTracked = OnSoundPlayedHandler::g_trackedSounds;

            for (const auto& updated : updatedSounds) {
                for (auto& live : liveTracked) {
                    if (isSameTracked(live, updated)) {
                        live.handleState = updated.handleState;
                        live.hasEverPlayed = updated.hasEverPlayed;
                        live.pollCount = updated.pollCount;
                        break;
                    }
                }
            }

            if (!completedSounds.empty()) {
                liveTracked.erase(
                    std::remove_if(liveTracked.begin(), liveTracked.end(),
                        [&](const TrackedVoiceSound& live) {
                            for (const auto& completed : completedSounds)
                                if (isSameTracked(live, completed)) return true;
                            return false;
                        }),
                    liveTracked.end());
            }
        }
    }
}

bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    EnsureStateLockInitialized();
    OnSoundPlayedHandler::g_mainThreadId = GetCurrentThreadId();
    s_pluginHandle = nvse->GetPluginHandle ? nvse->GetPluginHandle() : kPluginHandle_Invalid;

    //prologue: push ebp (1) + mov ebp,esp (2) + push -1 (2) = 5 bytes
    if (!s_detour.WriteRelJump(0xAE5A50, HookedGetSoundHandle, 5))
        return false;

    OnSoundPlayedHandler::g_hookInstalled = true;
    return true;
}
}
