//inject arbitrary wav/ogg/mp3 tracks into the pip-boy/world radio stream
//substitutes the filename arg at the four sub_AD7480 (GetSoundHandleByFilename wrapper)
//call sites inside FalloutRadio::PipboyUpdate and FalloutRadio::UpdateStation,
//which bypasses the engine's _mono/_stereo mangling and the form-path lookup,
//and rewrites the path buffer at the PipboyUpdate song call to PlayingMusic::SetPlayingMusic,
//the mp3 media streamer that carries every pip-boy song

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

//ExtractArgsEx resolves string_var args, the raw 0x5ACCB0 extractor hands them over as literal text
#define EXTRACT_ARGS_EX paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList
static bool (*ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;

namespace RadioInjection {

static const int kMaxQueue = 8;
static const int kPathLen = 240;

static char s_queue[kMaxQueue][kPathLen];
//wav/ogg suits both the world speakers and the pip-boy's spoken segment, this keeps an
//entry meant for speakers from being handed to the segment request and eating DJ chatter
static bool s_speakersOnly[kMaxQueue] = {};
static int s_head = 0;
static int s_count = 0;
static char s_activePath[256];
static RadioInjectionLogic::SlotFormat s_activeSlot = RadioInjectionLogic::kSlot_Sound;

static CRITICAL_SECTION s_lock;
static volatile LONG s_lockInit = 0;
static DWORD s_lastPopTick = 0;
static bool s_loopMode = false;

static const char kTrackChangeEvent[] = "ITR:OnRadioTrackChange";

//one logical track advance creates several sounds (pip-boy plus one per placed world
//speaker), all within the same update burst. pop once, then reuse the same path for the
//rest of the burst so one injected track feeds every speaker of that slot consistently
static const DWORD kSameTrackWindowMs = 500;

static void EnsureLockInit()
{
	InitCriticalSectionOnce(&s_lockInit, &s_lock);
}

//removes the entry at logical offset from the head, shifting the entries before it
//one slot toward the tail so remaining order is preserved, then advances the head
static void TakeAt_Unlocked(int offset, char* dst, size_t dstSize, bool* dstSpeakersOnly = nullptr)
{
	const char* src = s_queue[(s_head + offset) % kMaxQueue];
	size_t n = strlen(src);
	if (n >= dstSize) n = dstSize - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
	if (dstSpeakersOnly)
		*dstSpeakersOnly = s_speakersOnly[(s_head + offset) % kMaxQueue];
	for (int j = offset; j > 0; j--)
	{
		int to = (s_head + j) % kMaxQueue;
		int from = (s_head + j - 1) % kMaxQueue;
		strcpy_s(s_queue[to], kPathLen, s_queue[from]);
		s_speakersOnly[to] = s_speakersOnly[from];
	}
	s_head = (s_head + 1) % kMaxQueue;
	s_count--;
}

static bool PushBack_Unlocked(const char* path, bool speakersOnly = false)
{
	if (s_count >= kMaxQueue)
		return false;
	int slot = (s_head + s_count) % kMaxQueue;
	strncpy_s(s_queue[slot], kPathLen, path, _TRUNCATE);
	s_speakersOnly[slot] = speakersOnly;
	s_count++;
	return true;
}

//play-now takes priority, evicts the tail when the ring is full
static void PushFront_Unlocked(const char* path, bool speakersOnly = false)
{
	s_head = (s_head + kMaxQueue - 1) % kMaxQueue;
	strncpy_s(s_queue[s_head], kPathLen, path, _TRUNCATE);
	s_speakersOnly[s_head] = speakersOnly;
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

//one event per output that attempts to start a track, collapsing repeats of the same path in the frame.
//UpdateStations walks every station per frame and the tuned station reaches both the speakers
//(_mono path) and the pip-boy (original path), so those carry separate paths and fire separately
static void QueueTrackChange(const char* usePath, bool wasInjected)
{
	if (!usePath)
		return;

	ScopedLock lock(&s_lock);
	if (s_pendingEventCount >= kMaxPendingEvents)
		return;

	for (int i = 0; i < s_pendingEventCount; i++)
		if (strcmp(s_pendingEvents[i].path, usePath) == 0)
			return;

	strncpy_s(s_pendingEvents[s_pendingEventCount].path, usePath, _TRUNCATE);
	s_pendingEvents[s_pendingEventCount].wasInjected = wasInjected ? 1 : 0;
	s_pendingEventCount++;
}

//hands out the engine-ready path for this slot, taking the first queued entry whose
//format matches the slot so mixed-format queues feed both outputs without one
//stalling behind the other. an mp3 still waits for the stream slot rather than being
//swallowed by a sound handle that would rewrite its extension
static bool TakeTrackForSlot(RadioInjectionLogic::SlotFormat slot, char* out, size_t outSize, bool isSpeaker = true)
{
	ScopedLock lock(&s_lock);

	DWORD now = GetTickCount();
	if (s_lastPopTick && (now - s_lastPopTick) < kSameTrackWindowMs && s_activePath[0] && s_activeSlot == slot)
	{
		strncpy_s(out, outSize, s_activePath, _TRUNCATE);
		return true;
	}

	int match = -1;
	for (int i = 0; i < s_count; i++)
	{
		int idx = (s_head + i) % kMaxQueue;
		if (!RadioInjectionLogic::PathSuitsSlot(s_queue[idx], slot))
			continue;
		if (s_speakersOnly[idx] && !isSpeaker)
			continue;
		match = i;
		break;
	}
	if (match < 0)
		return false;

	char queuedPath[kPathLen];
	bool queuedSpeakersOnly = false;
	TakeAt_Unlocked(match, queuedPath, sizeof(queuedPath), &queuedSpeakersOnly);
	//loop mode: taken entry rotates to the tail so the playlist cycles until cleared
	if (s_loopMode)
		PushBack_Unlocked(queuedPath, queuedSpeakersOnly);

	if (!RadioInjectionLogic::BuildEnginePath(queuedPath, s_activePath, sizeof(s_activePath)))
		return false;

	s_lastPopTick = now;
	s_activeSlot = slot;
	strncpy_s(out, outSize, s_activePath, _TRUNCATE);
	return true;
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
//a reentrant engine call would consume a second queue entry for the same sound,
//guard so a nested call passes through untouched. main thread only, plain bool suffices
static bool s_inSubstitute = false;

static void* GetSoundHandleWork(GetSoundHandle_t orig, void* thisPtr, void* edx, void* handleOut, const char* filename, UInt32 flags, void* baseForm, bool isSpeaker, int site)
{
	if (s_inSubstitute)
		return orig ? orig(thisPtr, edx, handleOut, filename, flags, baseForm) : nullptr;

	char injected[sizeof(s_activePath)];
	const char* usePath = filename;
	const bool isVoice = RadioInjectionLogic::IsVoicePath(filename);
	const bool wasInjected = !isVoice
		&& TakeTrackForSlot(RadioInjectionLogic::kSlot_Sound, injected, sizeof(injected), isSpeaker);
	Log("RadioInjection: SOUND site=%d speaker=%d voice=%d queue=%d req='%s'", site, isSpeaker ? 1 : 0, isVoice ? 1 : 0, s_count, filename ? filename : "");
	if (wasInjected)
	{
		usePath = injected;
		Log("RadioInjection: substituting radio track '%s'", usePath);
	}

	s_inSubstitute = true;
	QueueTrackChange(usePath, wasInjected);
	void* ret = orig ? orig(thisPtr, edx, handleOut, usePath, flags, wasInjected ? nullptr : baseForm) : nullptr;
	s_inSubstitute = false;
	return ret;
}

//kSites 0-1 are the spoken segment calls inside PipboyUpdate, 2-3 the speaker calls
//inside UpdateStation, so the site the request came through identifies the output
#define SOUND_THUNK(N, SPEAKER) static void* __fastcall Hook_GetSoundHandle_##N(void* t, void* e, void* h, const char* f, UInt32 fl, void* b) \
	{ return GetSoundHandleWork((GetSoundHandle_t)s_calls[N].GetOverwrittenAddr(), t, e, h, f, fl, b, SPEAKER, N); }
SOUND_THUNK(0, false) SOUND_THUNK(1, false) SOUND_THUNK(2, true) SOUND_THUNK(3, true)
#undef SOUND_THUNK
static void* const kSoundThunks[4] = {
	(void*)Hook_GetSoundHandle_0, (void*)Hook_GetSoundHandle_1,
	(void*)Hook_GetSoundHandle_2, (void*)Hook_GetSoundHandle_3,
};

//PlayingMusic::SetPlayingMusic, __cdecl with 7 dword args
typedef int (__cdecl* SetPlayingMusic_t)(UInt32 musicType, char* path, UInt32 fadeMS, UInt32 a4, UInt32 a5, float a6, UInt32 a7);
static Detours::CallDetour s_streamCall;

//capacity the engine formats the song path with, and copies out of into g_currentRadioSong
static const int kStreamPathLen = 260;

static int __cdecl Hook_SetPlayingMusic(UInt32 musicType, char* path, UInt32 fadeMS, UInt32 a4, UInt32 a5, float a6, UInt32 a7)
{
	SetPlayingMusic_t orig = (SetPlayingMusic_t)s_streamCall.GetOverwrittenAddr();

	if (path)
	{
		char injected[sizeof(s_activePath)];
		//the buffer is the caller's stack local, rewrite in place or the radio commands
		//keep reporting the vanilla song
		Log("RadioInjection: SONG queue=%d req='%s'", s_count, path);
		//the engine asks both outputs for every entry, but the streamer only opens mp3,
		//so a request for anything else is silent and the audio comes from the paired
		//sound handle. injecting into that dead request adds a second audible track
		//a request the streamer cannot open is silent and the audio comes from the paired
		//sound handle, which reports itself, so this one is neither injected nor announced
		const bool canStream = RadioInjectionLogic::PathSuitsSlot(path, RadioInjectionLogic::kSlot_Stream);
		if (canStream)
		{
			if (TakeTrackForSlot(RadioInjectionLogic::kSlot_Stream, injected, sizeof(injected)))
			{
				strncpy_s(path, kStreamPathLen, injected, _TRUNCATE);
				Log("RadioInjection: substituting radio song '%s'", path);
				QueueTrackChange(path, true);
			}
			else
				QueueTrackChange(path, false);
		}
	}

	return orig ? orig(musicType, path, fadeMS, a4, a5, a6, a7) : 0;
}

void Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse && nvse->isEditor) return;

	if (nvse)
	{
		NVSEScriptInterface* scriptInterface = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
		if (scriptInterface)
			ExtractArgsEx = scriptInterface->ExtractArgsEx;
	}

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

	//only call to SetPlayingMusic in PipboyUpdate carrying a path, the rest are stops
	if (s_streamCall.WriteRelCall(0x83398D, (UInt32)Hook_SetPlayingMusic))
		installed++;
	else
		Log("RadioInjection: ERROR expected E8 call at 0x0083398D");

	Log("RadioInjection: %d/5 call sites hooked", installed);
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

	if (!RadioInjectionLogic::PathSuitsSlot(path, RadioInjectionLogic::kSlot_Sound) &&
		!RadioInjectionLogic::PathSuitsSlot(path, RadioInjectionLogic::kSlot_Stream))
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
	if (!ExtractArgsEx || !ExtractArgsEx(EXTRACT_ARGS_EX, &path))
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
	UInt32 speakersOnly = 0;
	if (!ExtractArgsEx || !ExtractArgsEx(EXTRACT_ARGS_EX, &path, &speakersOnly))
		return true;

	if (!IsValidRadioPath(path))
	{
		Log("RadioInjection: QueueRadioTrack rejected path '%s'", path);
		return true;
	}

	bool queued;
	{
		ScopedLock lock(&s_lock);
		queued = PushBack_Unlocked(path, speakersOnly != 0);
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
	if (!ExtractArgsEx || !ExtractArgsEx(EXTRACT_ARGS_EX, &enable))
		return true;

	{
		ScopedLock lock(&s_lock);
		s_loopMode = (enable != 0);
	}
	*result = 1;
	return true;
}

//an entry only leaves through the slot its extension suits, so a caller topping up both
//outputs needs to ask about each one separately, a total would hide a stranded entry
static bool Cmd_GetRadioQueueSize_Execute(COMMAND_ARGS)
{
	UInt32 slot = 0;
	if (ExtractArgsEx && !ExtractArgsEx(EXTRACT_ARGS_EX, &slot))
		slot = 0;

	int count = 0;
	{
		ScopedLock lock(&s_lock);
		if (slot != 1 && slot != 2)
			count = s_count;
		else
		{
			RadioInjectionLogic::SlotFormat want = (slot == 1)
				? RadioInjectionLogic::kSlot_Stream
				: RadioInjectionLogic::kSlot_Sound;
			for (int i = 0; i < s_count; i++)
				if (RadioInjectionLogic::PathSuitsSlot(s_queue[(s_head + i) % kMaxQueue], want))
					count++;
		}
	}
	*result = count;
	return true;
}

static ParamInfo kParams_OneInt[1] = {
	{"enable", kParamType_Integer, 0}
};

static ParamInfo kParams_OneOptionalInt[1] = {
	{"slot", kParamType_Integer, 1}
};

static ParamInfo kParams_OneString[1] = {
	{"path", kParamType_String, 0}
};

static ParamInfo kParams_OneString_OneOptionalInt[2] = {
	{"path", kParamType_String, 0},
	{"bSpeakersOnly", kParamType_Integer, 1}
};

static CommandInfo kCommandInfo_PlayRadioFile = {
	"PlayRadioFile", "", 0,
	"Skips the current radio track and plays a wav/ogg/mp3 file (relative to Data\\Sound\\) next. mp3 files play on the song slot, wav/ogg on the spoken segment slot, so an mp3 waits for the next song. Subtitles are unavailable for injected files.",
	0, 1, kParams_OneString, Cmd_PlayRadioFile_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_QueueRadioTrack = {
	"QueueRadioTrack", "", 0,
	"Queues a wav/ogg/mp3 file (relative to Data\\Sound\\) to play on the next natural track advance. mp3 files play on the song slot, wav/ogg on the spoken segment slot. Pass 1 for bSpeakersOnly to keep a wav/ogg off the pip-boy's spoken segment so it only plays through world radios. Returns 0 if the queue is full. Subtitles are unavailable for injected files.",
	0, 2, kParams_OneString_OneOptionalInt, Cmd_QueueRadioTrack_Execute, nullptr, nullptr, 0
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
	"GetRadioQueueSize", "", 0,
	"Returns the number of tracks currently in the injected radio queue. Pass 1 to count only entries for the song slot (mp3) or 2 for the spoken segment slot (wav/ogg); omit or pass 0 for the total.",
	0, 1, kParams_OneOptionalInt, Cmd_GetRadioQueueSize_Execute, nullptr, nullptr, 0
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
}

void ClearState()
{
	EnsureLockInit();
	ScopedLock lock(&s_lock);
	s_head = 0;
	s_count = 0;
	s_lastPopTick = 0;
	s_activePath[0] = '\0';
	s_activeSlot = RadioInjectionLogic::kSlot_Sound;
	s_loopMode = false;
	s_pendingEventCount = 0;
}

}
