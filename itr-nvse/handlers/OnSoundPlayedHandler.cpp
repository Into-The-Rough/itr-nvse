//hooks BSAudioManager::GetSoundHandleByFilePath at 0xAE5A50 to catch ALL sounds
//uses a queue to dispatch events on the main thread (audio may run on separate thread)

#include <cstring>
#include <Windows.h>

#include "OnSoundPlayedHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/EventDispatch.h"
#include "internal/GameSDK.h"


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
    UInt32 soundID;
    float x;
    float y;
    float z;
    float volume;
    UInt8 hasPos;
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

static constexpr UInt32 kMaxQueueSize = 256;
static constexpr UInt32 kMaxTrackedSounds = 64;
static constexpr UInt32 kMaxPollsWithoutPlayback = 180;

namespace OnSoundPlayedHandler {
    bool g_hookInstalled = false;
    QueuedSoundEvent g_pendingEvents[kMaxQueueSize];
    UInt32 g_pendingCount = 0;
    CRITICAL_SECTION g_stateLock;
    volatile LONG g_stateLockInit = 0;
    TrackedVoiceSound g_trackedSounds[kMaxTrackedSounds];
    UInt32 g_trackedCount = 0;
    DWORD g_mainThreadId = 0;
}

static constexpr const char* kSoundPlayedEvent = "ITR:OnSoundPlayed";
static constexpr const char* kSoundCompletedEvent = "ITR:OnSoundCompleted";
static constexpr const char* kSoundPosEvent = "ITR:OnSoundPlayedByPosition";
static constexpr int kProbePriority = -9999;
static constexpr UInt32 kListenerProbeIntervalFrames = 30;

static PluginHandle s_pluginHandle = kPluginHandle_Invalid;
static bool s_probeHandlersInstalled = false;
static volatile bool s_hasPlayedListeners = true;
static volatile bool s_hasCompletedListeners = true;
static volatile bool s_hasPosListeners = true;
static UInt32 s_listenerProbeFrame = kListenerProbeIntervalFrames;

static void SoundPlayedProbeHandler(TESObjectREFR*, void*) {}
static void SoundCompletedProbeHandler(TESObjectREFR*, void*) {}
static void SoundPosProbeHandler(TESObjectREFR*, void*) {}

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
    s_hasPosListeners = HasExternalHandlers(kSoundPosEvent, SoundPosProbeHandler);
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

static void QueueSoundEvent(const char* filePath, UInt32 flags, UInt32 soundFormID, UInt32 soundID)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;

    QueuedSoundEvent evt;
    if (filePath && filePath[0])
        strncpy_s(evt.filePath, sizeof(evt.filePath), filePath, _TRUNCATE);
    else
        evt.filePath[0] = '\0';
    evt.soundFlags = flags;
    evt.soundFormID = soundFormID;
    evt.soundID = soundID;
    evt.x = evt.y = evt.z = 0.0f;
    evt.volume = 1.0f;
    evt.hasPos = 0;

    ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
    if (OnSoundPlayedHandler::g_pendingCount >= kMaxQueueSize) return;
    OnSoundPlayedHandler::g_pendingEvents[OnSoundPlayedHandler::g_pendingCount++] = evt;
}

static void UpdatePendingPos(UInt32 soundID, float x, float y, float z)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
    for (UInt32 i = OnSoundPlayedHandler::g_pendingCount; i-- > 0;) {
        QueuedSoundEvent& evt = OnSoundPlayedHandler::g_pendingEvents[i];
        if (evt.soundID != soundID) continue;
        evt.x = x;
        evt.y = y;
        evt.z = z;
        evt.hasPos = 1;
        break;
    }
}

static void UpdatePendingVol(UInt32 soundID, float volume)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    ScopedLock lock(&OnSoundPlayedHandler::g_stateLock);
    for (UInt32 i = OnSoundPlayedHandler::g_pendingCount; i-- > 0;) {
        QueuedSoundEvent& evt = OnSoundPlayedHandler::g_pendingEvents[i];
        if (evt.soundID != soundID) continue;
        evt.volume = volume;
        break;
    }
}

static void QueueVoiceTracking(UInt32 soundID, const char* filePath, UInt32 flags, UInt32 soundFormID, const BSSoundHandle* handle)
{
    if (OnSoundPlayedHandler::g_stateLockInit != 2) return;
    if (soundID == 0 || soundID == 0xFFFFFFFF) return;

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
    if (OnSoundPlayedHandler::g_trackedCount >= kMaxTrackedSounds) return;
    OnSoundPlayedHandler::g_trackedSounds[OnSoundPlayedHandler::g_trackedCount++] = tracked;
}

enum { kSoundFlag_IsVoice = 0x4 };

static BSSoundHandle* __fastcall HookedGetSoundHandle(
    BSAudioManager* mgr, void* edx,
    BSSoundHandle* arData, const char* apName,
    UInt32 aeAudioFlags, TESSound* apSound)
{
    bool hasPlayedListeners = s_hasPlayedListeners;
    bool hasPosListeners = s_hasPosListeners;
    bool hasCompletedListeners = s_hasCompletedListeners;
    UInt32 soundFormID = ReadRefID(apSound);
    bool hasEventManager = g_eventManagerInterface != nullptr;

    BSSoundHandle* result = s_detour.GetTrampoline<GetSoundHandleByFilePath_t>()(mgr, arData, apName, aeAudioFlags, apSound);

    //queue after the original so the entry carries the sound ID for position/volume correlation
    if (hasEventManager && (hasPlayedListeners || hasPosListeners) && apName && apName[0])
        QueueSoundEvent(apName, aeAudioFlags, soundFormID, result ? result->uiSoundID : 0xFFFFFFFF);

    if (hasEventManager && hasCompletedListeners && (aeAudioFlags & kSoundFlag_IsVoice) && apName && apName[0]) {
        if (result && result->uiSoundID != 0 && result->uiSoundID != 0xFFFFFFFF)
            QueueVoiceTracking(result->uiSoundID, apName, aeAudioFlags, soundFormID, result);
    }

    return result;
}

typedef char(__thiscall* SetPosition_t)(void* handle, float x, float y, float z);
typedef char(__thiscall* SetVolume_t)(void* handle, float volume);
static Detours::JumpDetour s_posDetour;
static Detours::JumpDetour s_volDetour;

static char __fastcall HookedSetPosition(UInt32* handle, void*, float x, float y, float z)
{
    if (s_hasPosListeners && handle && *handle != 0xFFFFFFFF)
        UpdatePendingPos(*handle, x, y, z);
    return s_posDetour.GetTrampoline<SetPosition_t>()(handle, x, y, z);
}

static char __fastcall HookedSetVolume(UInt32* handle, void*, float volume)
{
    if (s_hasPosListeners && handle && *handle != 0xFFFFFFFF)
        UpdatePendingVol(*handle, volume);
    return s_volDetour.GetTrampoline<SetVolume_t>()(handle, volume);
}

static bool IsSameTracked(const TrackedVoiceSound& a, const TrackedVoiceSound& b)
{
    return a.soundID == b.soundID && a.soundFormID == b.soundFormID &&
           std::strcmp(a.filePath, b.filePath) == 0;
}

namespace OnSoundPlayedHandler {
void InstallListenerProbes()
{
    bool playedProbe = InstallProbeHandler(kSoundPlayedEvent, SoundPlayedProbeHandler, "ITR_OnSoundPlayedProbe");
    bool completedProbe = InstallProbeHandler(kSoundCompletedEvent, SoundCompletedProbeHandler, "ITR_OnSoundCompletedProbe");
    bool posProbe = InstallProbeHandler(kSoundPosEvent, SoundPosProbeHandler, "ITR_OnSoundPlayedByPositionProbe");

    s_probeHandlersInstalled = playedProbe && completedProbe && posProbe;
    s_listenerProbeFrame = kListenerProbeIntervalFrames;
    s_hasPlayedListeners = true;
    s_hasCompletedListeners = true;
    s_hasPosListeners = true;
}

void Update()
{
    if (g_stateLockInit != 2) return;
    if (!g_eventManagerInterface) return;

    DWORD currentThreadId = GetCurrentThreadId();
    if (!g_mainThreadId)
        g_mainThreadId = currentThreadId;
    if (currentThreadId != g_mainThreadId)
        return;

    RefreshListenerState(false);

    static QueuedSoundEvent s_drainEvents[kMaxQueueSize];
    static TrackedVoiceSound s_snapshot[kMaxTrackedSounds];
    UInt32 drainCount, snapCount;
    {
        ScopedLock lock(&g_stateLock);
        drainCount = g_pendingCount;
        if (drainCount)
            memcpy(s_drainEvents, g_pendingEvents, drainCount * sizeof(QueuedSoundEvent));
        g_pendingCount = 0;
        snapCount = g_trackedCount;
        if (snapCount)
            memcpy(s_snapshot, g_trackedSounds, snapCount * sizeof(TrackedVoiceSound));
    }

    for (UInt32 i = 0; i < drainCount; ++i)
    {
        const QueuedSoundEvent& evt = s_drainEvents[i];
        //sounds played by file path carry no TESSound form, dispatch them with a null source
        TESForm* sourceSound = evt.soundFormID ? (TESForm*)Engine::LookupFormByID(evt.soundFormID) : nullptr;

        if (s_hasPlayedListeners)
            g_eventManagerInterface->DispatchEvent(kSoundPlayedEvent, nullptr,
                evt.filePath, (int)evt.soundFlags, (TESObjectREFR*)sourceSound);

        if (s_hasPosListeners)
            g_eventManagerInterface->DispatchEvent(kSoundPosEvent, nullptr,
                evt.filePath, (int)evt.soundFlags, (TESObjectREFR*)sourceSound,
                (int)evt.hasPos, PackEventFloatArg(evt.x), PackEventFloatArg(evt.y),
                PackEventFloatArg(evt.z), PackEventFloatArg(evt.volume));
    }

    if (!snapCount)
        return;

    //poll outside the lock, IsPlaying may take audio locks the hook thread holds
    UInt8 action[kMaxTrackedSounds]; //0 keep, 1 remove, 2 remove and dispatch completed
    for (UInt32 i = 0; i < snapCount; ++i)
    {
        TrackedVoiceSound& tracked = s_snapshot[i];
        ++tracked.pollCount;
        bool stillPlaying = Engine::BSSoundHandle_IsPlaying(&tracked.handleState);
        if (stillPlaying)
            tracked.hasEverPlayed = true;

        if (!stillPlaying && tracked.hasEverPlayed)
            action[i] = 2;
        else if (!tracked.hasEverPlayed && tracked.pollCount >= kMaxPollsWithoutPlayback)
            action[i] = 1;
        else
            action[i] = 0;
    }

    {
        ScopedLock lock(&g_stateLock);
        for (UInt32 i = 0; i < snapCount; ++i)
        {
            //match by identity, the hook thread may have appended or ClearState wiped the array
            for (UInt32 j = 0; j < g_trackedCount; ++j)
            {
                TrackedVoiceSound& live = g_trackedSounds[j];
                if (!IsSameTracked(live, s_snapshot[i])) continue;
                if (action[i])
                    g_trackedSounds[j] = g_trackedSounds[--g_trackedCount];
                else {
                    live.handleState = s_snapshot[i].handleState;
                    live.hasEverPlayed = s_snapshot[i].hasEverPlayed;
                    live.pollCount = s_snapshot[i].pollCount;
                }
                break;
            }
        }
    }

    for (UInt32 i = 0; i < snapCount; ++i)
    {
        if (action[i] != 2) continue;
        const TrackedVoiceSound& tracked = s_snapshot[i];
        //voice lines played by file path carry no TESSound form, dispatch with a null source
        TESForm* sourceSound = tracked.soundFormID ? (TESForm*)Engine::LookupFormByID(tracked.soundFormID) : nullptr;

        g_eventManagerInterface->DispatchEvent(kSoundCompletedEvent, nullptr,
            tracked.filePath, (int)tracked.soundFlags, (TESObjectREFR*)sourceSound);
    }
}

void ClearState()
{
    if (g_stateLockInit != 2) return;
    ScopedLock lock(&g_stateLock);
    g_pendingCount = 0;
    g_trackedCount = 0;
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

    //0xAD8B60 = BSSoundHandle::SetPosition, 0xAD89E0 = BSSoundHandle::SetVolume, sole routes to the by-ID workers
    //prologue both: push ebp (1) + mov ebp,esp (2) + push ecx (1) + mov [ebp-4],ecx (3) = 7 bytes
    s_posDetour.WriteRelJump(0xAD8B60, HookedSetPosition, 7);
    s_volDetour.WriteRelJump(0xAD89E0, HookedSetVolume, 7);

    OnSoundPlayedHandler::g_hookInstalled = true;
    return true;
}
}
