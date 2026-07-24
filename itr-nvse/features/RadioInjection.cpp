//inject arbitrary wav/ogg tracks into the pip-boy/world radio stream
//substitutes the filename arg at the four sub_AD7480 (GetSoundHandleByFilename wrapper)
//call sites inside FalloutRadio::PipboyUpdate and FalloutRadio::UpdateStation,
//which bypasses the engine's _mono/_stereo mangling and the form-path lookup

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "RadioInjection.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/globals.h"
#include "internal/ScopedLock.h"
#include "internal/EventDispatch.h"
#include "internal/RadioInjectionLogic.h"

#define EXTRACT_ARGS paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList
typedef bool (*ExtractArgs_t)(ParamInfo*, void*, UInt32*, TESObjectREFR*, TESObjectREFR*, Script*, ScriptEventList*, ...);
static ExtractArgs_t ExtractArgs = (ExtractArgs_t)0x5ACCB0;

namespace RadioInjection {

static const int kMaxQueue = 8;
static const int kPathLen = 240;

static char s_queue[kMaxQueue][kPathLen];
static int s_head = 0;
static int s_count = 0;
static char s_activePath[256]; //filled under lock, engine strcpy's it immediately

static CRITICAL_SECTION s_lock;
static volatile LONG s_lockInit = 0;
static DWORD s_lastPopTick = 0;
static bool s_loopMode = false;

//track-change dedup, one logical advance dispatches once for the whole speaker burst
static char s_lastDispatchedPath[256] = {};
static DWORD s_lastDispatchTick = 0;

static const char kTrackChangeEvent[] = "ITR:OnRadioTrackChange";

//one logical track advance creates several sounds (pip-boy stream plus one per placed
//world speaker), all within the same update burst. pop once, then reuse the same path
//for the rest of the burst so one injected track feeds every speaker consistently
static const DWORD kSameTrackWindowMs = 500;

static void EnsureLockInit()
{
	InitCriticalSectionOnce(&s_lockInit, &s_lock);
}

static void PopFront_Unlocked(char* dst, size_t dstSize)
{
	const char* src = s_queue[s_head];
	size_t n = strlen(src);
	if (n >= dstSize) n = dstSize - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
	s_head = (s_head + 1) % kMaxQueue;
	s_count--;
}

//loop mode: hand out the front path but rotate it to the tail instead of consuming,
//so the playlist cycles until cleared or loop is disabled. count unchanged
static void RotateFront_Unlocked(char* dst, size_t dstSize)
{
	const char* src = s_queue[s_head];
	size_t n = strlen(src);
	if (n >= dstSize) n = dstSize - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
	int tailSlot = (s_head + s_count) % kMaxQueue;   //slot the front rotates into once head advances
	if (tailSlot != s_head)
		strcpy_s(s_queue[tailSlot], kPathLen, s_queue[s_head]);
	s_head = (s_head + 1) % kMaxQueue;
}

static bool PushBack_Unlocked(const char* path)
{
	if (s_count >= kMaxQueue)
		return false;
	int slot = (s_head + s_count) % kMaxQueue;
	strncpy_s(s_queue[slot], kPathLen, path, _TRUNCATE);
	s_count++;
	return true;
}

//play-now takes priority, evicts the tail when the ring is full
static void PushFront_Unlocked(const char* path)
{
	s_head = (s_head + kMaxQueue - 1) % kMaxQueue;
	strncpy_s(s_queue[s_head], kPathLen, path, _TRUNCATE);
	if (s_count < kMaxQueue)
		s_count++;
}

//ecx=BSWin32Audio* this, arg_4 (second stack arg) is the filename passed straight to
//BSAudioManager::GetSoundHandleByFilename, all four hooked sites share this form
typedef void* (__fastcall* GetSoundHandle_t)(void* thisPtr, void* edx, void* handleOut, const char* filename, UInt32 flags, void* baseForm);

static const UInt32 kSites[4] = { 0x833722, 0x833791, 0x8353B1, 0x835454 };
static Detours::CallDetour s_calls[4];

//the hook runs inside FalloutRadio::PipboyUpdate/UpdateStation, so events are not
//dispatched there. captured track changes queue as plain values and Update drains
//them on the main loop
struct PendingTrackChange {
	char path[256];
	int wasInjected;
};
static const int kMaxPendingEvents = 8;
static PendingTrackChange s_pendingEvents[kMaxPendingEvents];
static int s_pendingEventCount = 0;

//fire once per logical advance, not once per speaker in the burst. queue only when the
//used path differs from the last queued one or the burst window has expired
static void QueueTrackChange(const char* usePath, bool wasInjected)
{
	if (!usePath)
		return;

	DWORD now = GetTickCount();
	bool burstExpired = (now - s_lastDispatchTick) >= kSameTrackWindowMs;
	if (!burstExpired && strcmp(usePath, s_lastDispatchedPath) == 0)
		return;

	strncpy_s(s_lastDispatchedPath, usePath, _TRUNCATE);
	s_lastDispatchTick = now;

	ScopedLock lock(&s_lock);
	if (s_pendingEventCount >= kMaxPendingEvents)
		return;
	strncpy_s(s_pendingEvents[s_pendingEventCount].path, usePath, _TRUNCATE);
	s_pendingEvents[s_pendingEventCount].wasInjected = wasInjected ? 1 : 0;
	s_pendingEventCount++;
}

void Update()
{
	if (!s_pendingEventCount || !g_eventManagerInterface)
		return;

	PendingTrackChange local[kMaxPendingEvents];
	int n;
	{
		EnsureLockInit();
		ScopedLock lock(&s_lock);
		n = s_pendingEventCount;
		memcpy(local, s_pendingEvents, n * sizeof(PendingTrackChange));
		s_pendingEventCount = 0;
	}

	for (int i = 0; i < n; i++)
		g_eventManagerInterface->DispatchEvent(kTrackChangeEvent, nullptr, local[i].path, local[i].wasInjected);
}

//worker holds the shared substitution logic; per-site thunks pass their own recorded original
//so each site chains correctly even if another plugin patched one of the four separately
//a reentrant engine call would overwrite the shared s_activePath the outer call still holds,
//guard so a nested call passes through untouched. main thread only, plain bool suffices
static bool s_inSubstitute = false;

static void* GetSoundHandleWork(GetSoundHandle_t orig, void* thisPtr, void* edx, void* handleOut, const char* filename, UInt32 flags, void* baseForm)
{
	if (s_inSubstitute)
		return orig ? orig(thisPtr, edx, handleOut, filename, flags, baseForm) : nullptr;

	const char* usePath = filename;
	bool wasInjected = false;
	{
		ScopedLock lock(&s_lock);
		DWORD now = GetTickCount();
		if (s_lastPopTick && (now - s_lastPopTick) < kSameTrackWindowMs && s_activePath[0])
		{
			usePath = s_activePath; //same track burst, reuse without popping
			wasInjected = true;
		}
		else if (s_count > 0)
		{
			char queuedPath[kPathLen];
			if (s_loopMode)
				RotateFront_Unlocked(queuedPath, sizeof(queuedPath));
			else
				PopFront_Unlocked(queuedPath, sizeof(queuedPath));
			if (RadioInjectionLogic::BuildEnginePath(queuedPath, s_activePath, sizeof(s_activePath)))
			{
				s_lastPopTick = now;
				usePath = s_activePath;
				wasInjected = true;
			}
		}
	}

	if (wasInjected)
		Log("RadioInjection: substituting radio track '%s'", usePath);

	s_inSubstitute = true;
	QueueTrackChange(usePath, wasInjected);
	void* ret = orig ? orig(thisPtr, edx, handleOut, usePath, flags, wasInjected ? nullptr : baseForm) : nullptr;
	s_inSubstitute = false;
	return ret;
}

#define SOUND_THUNK(N) static void* __fastcall Hook_GetSoundHandle_##N(void* t, void* e, void* h, const char* f, UInt32 fl, void* b) \
	{ return GetSoundHandleWork((GetSoundHandle_t)s_calls[N].GetOverwrittenAddr(), t, e, h, f, fl, b); }
SOUND_THUNK(0) SOUND_THUNK(1) SOUND_THUNK(2) SOUND_THUNK(3)
#undef SOUND_THUNK
static void* const kSoundThunks[4] = {
	(void*)Hook_GetSoundHandle_0, (void*)Hook_GetSoundHandle_1,
	(void*)Hook_GetSoundHandle_2, (void*)Hook_GetSoundHandle_3,
};

void Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse && nvse->isEditor) return;

	EnsureLockInit();

	if (g_eventManagerInterface)
	{
		using P = NVSEEventManagerInterface::ParamType;
		using F = NVSEEventManagerInterface::EventFlags;
		static P params[] = { P::eParamType_String, P::eParamType_Int };
		g_eventManagerInterface->RegisterEvent(kTrackChangeEvent, 2, params, F::kFlag_FlushOnLoad);
	}
	else
		Log("RadioInjection: event manager not ready at Init");

	int installed = 0;
	for (int i = 0; i < 4; i++)
	{
		if (s_calls[i].WriteRelCall(kSites[i], (UInt32)kSoundThunks[i]))
			installed++;
		else
			Log("RadioInjection: ERROR expected E8 call at 0x%08X", kSites[i]);
	}

	Log("RadioInjection: %d/4 call sites hooked", installed);
}

static bool IsValidRadioPath(const char* path)
{
	if (!path || !path[0])
		return false;
	if (path[0] == '\\' || path[0] == '/')
		return false;
	if (strchr(path, ':'))
		return false;
	if (strstr(path, ".."))
		return false;
	if (strlen(path) >= kPathLen)
		return false;

	const char* dot = strrchr(path, '.');
	if (!dot)
		return false;
	if (_stricmp(dot, ".wav") != 0 && _stricmp(dot, ".ogg") != 0)
		return false;

	return true;
}

//dword 0x11DD42C holds the active pip-boy station, +0x10 is soundTimeRemaining ms
static bool SkipCurrentTrack()
{
	void* station = GetCurrentRadio();
	if (!station)
		return false;
	*(UInt32*)((char*)station + 0x10) = 1; //soundTimeRemaining, advances next update tick
	return true;
}

static bool Cmd_PlayRadioFile_Execute(COMMAND_ARGS)
{
	*result = 0;

	char path[kPathLen] = {};
	if (!ExtractArgs(EXTRACT_ARGS, &path))
		return true;

	if (!GetCurrentRadio())
	{
		Log("RadioInjection: PlayRadioFile rejected, no active pip-boy station");
		return true;
	}

	if (!IsValidRadioPath(path))
	{
		Log("RadioInjection: PlayRadioFile rejected path '%s'", path);
		return true;
	}

	{
		ScopedLock lock(&s_lock);
		PushFront_Unlocked(path);
		s_lastPopTick = 0; //new injection must pop fresh, never reuse the previous burst path
	}

	SkipCurrentTrack();
	*result = 1;
	return true;
}

static bool Cmd_QueueRadioTrack_Execute(COMMAND_ARGS)
{
	*result = 0;

	char path[kPathLen] = {};
	if (!ExtractArgs(EXTRACT_ARGS, &path))
		return true;

	if (!IsValidRadioPath(path))
	{
		Log("RadioInjection: QueueRadioTrack rejected path '%s'", path);
		return true;
	}

	bool queued;
	{
		ScopedLock lock(&s_lock);
		queued = PushBack_Unlocked(path);
	}

	if (!queued)
		Log("RadioInjection: QueueRadioTrack full, dropped '%s'", path);

	*result = queued ? 1 : 0;
	return true;
}

static bool Cmd_ClearRadioQueue_Execute(COMMAND_ARGS)
{
	int cleared;
	{
		ScopedLock lock(&s_lock);
		cleared = s_count;
		s_head = 0;
		s_count = 0;
	}
	*result = cleared;
	return true;
}

static bool Cmd_SetRadioQueueLoop_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 enable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &enable))
		return true;

	{
		ScopedLock lock(&s_lock);
		s_loopMode = (enable != 0);
	}
	*result = 1;
	return true;
}

static bool Cmd_GetRadioQueueSize_Execute(COMMAND_ARGS)
{
	int count;
	{
		ScopedLock lock(&s_lock);
		count = s_count;
	}
	*result = count;
	return true;
}

static ParamInfo kParams_OneInt[1] = {
	{"enable", kParamType_Integer, 0}
};

static ParamInfo kParams_OneString[1] = {
	{"path", kParamType_String, 0}
};

static CommandInfo kCommandInfo_PlayRadioFile = {
	"PlayRadioFile", "", 0,
	"Skips the current radio track and plays a wav/ogg file (relative to Data\\Sound\\) next. Subtitles are unavailable for injected files.",
	0, 1, kParams_OneString, Cmd_PlayRadioFile_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_QueueRadioTrack = {
	"QueueRadioTrack", "", 0,
	"Queues a wav/ogg file (relative to Data\\Sound\\) to play on the next natural track advance. Returns 0 if the queue is full. Subtitles are unavailable for injected files.",
	0, 1, kParams_OneString, Cmd_QueueRadioTrack_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_ClearRadioQueue = {
	"ClearRadioQueue", "", 0, "Clears the injected radio track queue, returns the number of entries removed.",
	0, 0, nullptr, Cmd_ClearRadioQueue_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_SetRadioQueueLoop = {
	"SetRadioQueueLoop", "", 0, "Enables (1) or disables (0) queue loop mode. When on, played tracks rotate to the back of the queue instead of being consumed.",
	0, 1, kParams_OneInt, Cmd_SetRadioQueueLoop_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_GetRadioQueueSize = {
	"GetRadioQueueSize", "", 0, "Returns the number of tracks currently in the injected radio queue.",
	0, 0, nullptr, Cmd_GetRadioQueueSize_Execute, nullptr, nullptr, 0
};

void RegisterCommands(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->RegisterCommand(&kCommandInfo_PlayRadioFile);
	nvseIntf->RegisterCommand(&kCommandInfo_QueueRadioTrack);
	nvseIntf->RegisterCommand(&kCommandInfo_ClearRadioQueue);
	nvseIntf->RegisterCommand(&kCommandInfo_SetRadioQueueLoop);
	nvseIntf->RegisterCommand(&kCommandInfo_GetRadioQueueSize);
}

void BeginTrackAdvance()
{
	EnsureLockInit();
	ScopedLock lock(&s_lock);
	s_lastPopTick = 0;
	s_activePath[0] = '\0';
	s_lastDispatchTick = 0;
}

void ClearState()
{
	EnsureLockInit();
	ScopedLock lock(&s_lock);
	s_head = 0;
	s_count = 0;
	s_lastPopTick = 0;
	s_activePath[0] = '\0';
	s_loopMode = false;
	s_lastDispatchedPath[0] = '\0';
	s_lastDispatchTick = 0;
	s_pendingEventCount = 0;
}

}
