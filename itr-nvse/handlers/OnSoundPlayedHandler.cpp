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
#include "internal/EventDispatch.h"

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

namespace OnSoundPlayedHandler {
    bool g_hookInstalled = false;
    std::vector<QueuedSoundEvent> g_pendingEvents;
    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;
    std::vector<TrackedVoiceSound> g_trackedSounds;
    DWORD g_mainThreadId = 0;
}

static void EnsureStateLockInitialized()
{
    InitCriticalSectionOnce(&OnSoundPlayedHandler::g_stateLockInit, &OnSoundPlayedHandler::g_stateLock);
}

static UInt32 ReadRefID(const void* form)
{
    return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
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
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        if (OnSoundPlayedHandler::g_pendingEvents.size() >= kMaxQueueSize) return;
    }

    QueuedSoundEvent evt;
    if (filePath && filePath[0])
        strncpy_s(evt.filePath, sizeof(evt.filePath), filePath, _TRUNCATE);
    else
        evt.filePath[0] = '\0';
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

    constexpr size_t kMaxTrackedSounds = 64;
    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        if (OnSoundPlayedHandler::g_trackedSounds.size() >= kMaxTrackedSounds) return;
    }

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

    {
        ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
        OnSoundPlayedHandler::g_trackedSounds.push_back(tracked);
    }
}

enum { kSoundFlag_IsVoice = 0x4 };

static BSSoundHandle* __fastcall HookedGetSoundHandle(
    BSAudioManager* mgr, void* edx,
    BSSoundHandle* arData, const char* apName,
    UInt32 aeAudioFlags, TESSound* apSound)
{
    UInt32 soundFormID = ReadRefID(apSound);
    bool hasEventManager = g_eventManagerInterface != nullptr;

    if (hasEventManager && apName && apName[0])
        QueueSoundEvent(apName, aeAudioFlags, soundFormID);

    BSSoundHandle* result = s_detour.GetTrampoline<GetSoundHandleByFilePath_t>()(mgr, arData, apName, aeAudioFlags, apSound);

    if (hasEventManager && (aeAudioFlags & kSoundFlag_IsVoice) && apName && apName[0]) {
        if (result && result->uiSoundID != 0 && result->uiSoundID != 0xFFFFFFFF)
            QueueVoiceTracking(result->uiSoundID, apName, aeAudioFlags, soundFormID, result);
    }

    return result;
}

namespace OnSoundPlayedHandler {
void Update()
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    if (!g_eventManagerInterface) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!OnSoundPlayedHandler::g_mainThreadId)
        OnSoundPlayedHandler::g_mainThreadId = currentThreadId;
    if (currentThreadId != OnSoundPlayedHandler::g_mainThreadId)
        return;

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

        g_eventManagerInterface->DispatchEvent("ITR:OnSoundPlayed", nullptr,
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
                    g_eventManagerInterface->DispatchEvent("ITR:OnSoundCompleted", nullptr,
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

    //prologue: push ebp (1) + mov ebp,esp (2) + push -1 (2) = 5 bytes
    if (!s_detour.WriteRelJump(0xAE5A50, HookedGetSoundHandle, 5))
        return false;

    OnSoundPlayedHandler::g_hookInstalled = true;
    return true;
}
}
