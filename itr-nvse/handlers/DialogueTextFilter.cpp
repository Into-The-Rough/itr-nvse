//dialogue text filter - hooks dialogue responses and dispatches events

#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <Windows.h>

#include "DialogueTextFilter.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/globals.h"

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
	void*   speakerAnimation;       //0x20 (SNAM)
	void*   listenerAnimation;      //0x24 (LNAM)
	TESTopicInfoResponse* next;     //0x28
};
static_assert(offsetof(TESTopicInfoResponse, responseText) == 0x18);

struct ModInfo {
	UInt8 pad00[0x20];
	char name[0x100];
};

struct QueuedDialogueEvent
{
	UInt32 speakerRefID;
	UInt32 topicInfoRefID;
	UInt32 topicRefID;
	DWORD dispatchAfterTick;
	UInt8 responseNum;
	float duration;
	char text[512];
};

struct ConfirmedSpeak
{
	UInt32 speakerRefID;
	UInt32 baseFormID;
	UInt8 responseNum;
	DWORD timestamp;
	char modName[64];
	char voicePath[260];
};

struct RecentSpeak
{
	UInt32 speakerRefID;
	UInt32 baseFormID;
	UInt8 responseNum;
	DWORD timestamp;
};

namespace DialogueTextFilter {
	std::vector<QueuedDialogueEvent> g_pendingEvents;
	std::vector<ConfirmedSpeak> g_confirmedSpeaks;
	std::vector<RecentSpeak> g_recentSpeaks; //dedup buffer for double SPEAK from engine
	std::vector<RecentSpeak> g_recentFallbacks; //tracks fallback dispatches to prevent normal re-dispatch
	std::unordered_map<UInt32, UInt32> g_spokenGreets; //speakerRefID -> baseFormID of last spoken line
	bool g_hookInstalled = false;
	DWORD g_mainThreadId = 0;
	bool g_suppressed = false;
}

static CRITICAL_SECTION g_dtfStateLock;
static volatile LONG g_dtfStateLockInit = 0;

static void EnsureStateLockInitialized()
{
	InitCriticalSectionOnce(&g_dtfStateLockInit, &g_dtfStateLock);
}

constexpr UInt32 kAddr_RunResult = 0x61F170;
constexpr UInt32 kAddr_RunResultBody = 0x61F176;
constexpr UInt32 kAddr_GetResponses = 0x61E780;

static UInt32 ReadRefID(const void* form)
{
	return form ? *(const UInt32*)((const UInt8*)form + 0x0C) : 0;
}

static bool ParseModNameFromVoicePath(const char* path, char* outName, size_t outSize) {
	const char* p = strstr(path, "Voice\\");
	if (!p) return false;
	p += 6;
	const char* end = strchr(p, '\\');
	if (!end) return false;
	size_t len = end - p;
	if (len >= outSize) len = outSize - 1;
	memcpy(outName, p, len);
	outName[len] = '\0';
	return len > 0;
}

static const char* GetModName(UInt8 modIndex) {
	void* dh = *(void**)0x11C3F2C;
	if (!dh || modIndex >= 0xFF) return nullptr;
	ModInfo* modInfo = *(ModInfo**)((UInt8*)dh + 0x21C + (modIndex * 4));
	if (!modInfo) return nullptr;
	return (const char*)((UInt8*)modInfo + 0x20);
}

static UInt8 FindModIndex(const char* modName) {
	if (!modName || !*modName) return 0xFF;
	for (UInt16 i = 0; i < 0xFF; i++) {
		const char* name = GetModName((UInt8)i);
		if (!name) continue;
		if (_stricmp(name, modName) == 0)
			return (UInt8)i;
	}
	return 0xFF;
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
                          UInt8 responseNum)
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

	if (responseNum == 0) responseNum = 1;

	//engine truncates quest+topic EDIDs if combined length > 25
	//quest gets max 10, topic gets max 15, topic gets leftover from quest
	char questBuf[264], topicBuf[264];
	strncpy_s(questBuf, questID, _TRUNCATE);
	strncpy_s(topicBuf, topicID, _TRUNCATE);
	size_t qLen = strlen(questBuf);
	size_t tLen = strlen(topicBuf);
	if (qLen + tLen > 25) {
		int topicMax = 15;
		if (qLen <= 10)
			topicMax = (int)(10 - qLen) + 15;
		else
			questBuf[10] = '\0';
		topicBuf[topicMax] = '\0';
	}

	sprintf_s(outPath, outSize,
	          "Data\\Sound\\Voice\\%s\\%s\\%s_%s_%08X_%u.ogg",
	          modName, voiceTypeID, questBuf, topicBuf, baseFormID, responseNum);

	return true;
}

//extract base formID and response number from engine voice path
//format: ...\QuestID_TopicID_FORMID_N.ogg
static bool ParseVoicePathIDs(const char* path, UInt32& outFormID, UInt8& outRespNum) {
	if (!path || !*path) return false;

	const char* p = strrchr(path, '\\');
	const char* filename = p ? p + 1 : path;
	const char* dot = strrchr(filename, '.');
	if (!dot) return false;

	const char* us2 = nullptr;
	const char* us1 = nullptr;
	for (const char* c = dot - 1; c >= filename; c--) {
		if (*c == '_') {
			if (!us2) us2 = c;
			else if (!us1) { us1 = c; break; }
		}
	}
	if (!us1 || !us2) return false;

	size_t hexLen = us2 - (us1 + 1);
	if (hexLen == 0 || hexLen > 8) return false;
	char hexBuf[12] = {};
	memcpy(hexBuf, us1 + 1, hexLen);
	outFormID = (UInt32)strtoul(hexBuf, nullptr, 16);

	size_t numLen = dot - (us2 + 1);
	if (numLen == 0 || numLen > 3) return false;
	char numBuf[8] = {};
	memcpy(numBuf, us2 + 1, numLen);
	outRespNum = (UInt8)atoi(numBuf);

	return outFormID != 0;
}

namespace DialogueTextFilter {
void Suppress(bool suppress) {
	g_suppressed = suppress;
}
}

//check if this RunResult call is a false positive from a script chain during greeting evaluation.
//when ProcessGreet is active, HighProcess+0x3E4 (pQueuedGreetTopic) holds the REAL DialogueItem.
//any RunResult for a DIFFERENT topicInfo is a side effect — skip it.
//however, if SpeakSound has already confirmed this line is playing, it's real regardless.
static bool IsGreetingFalsePositive(Actor* speaker, UInt32 topicInfoRefID, UInt32 speakerRefID) {
	void* baseProcess = *(void**)((UInt8*)speaker + 0x68); //MobileObject+0x68
	if (!baseProcess) return false;

	int processLevel = *(int*)((UInt8*)baseProcess + 0x28); //BaseProcess+0x28
	if (processLevel != 0) return false; //not HighProcess

	void* queuedGreet = *(void**)((UInt8*)baseProcess + 0x3E4); //HighProcess+0x3E4 = pQueuedGreetTopic (DialogueItem*)
	if (!queuedGreet) {
		Log("DTF: speaker=0x%08X no pQueuedGreetTopic (non-greeting path), topicInfo=0x%08X -> ALLOW",
			speakerRefID, topicInfoRefID);
		return false;
	}

	void* activeTopicInfo = *(void**)((UInt8*)queuedGreet + 0x0C); //DialogueItem+0x0C = pTopicInfo
	UInt32 activeInfoRefID = activeTopicInfo ? ReadRefID(activeTopicInfo) : 0;

	if (activeInfoRefID && activeInfoRefID != topicInfoRefID) {
		//before rejecting, check if the active greet has already been spoken.
		//if so, pQueuedGreetTopic is stale and this is a new legitimate line.
		UInt32 activeBaseFormID = activeInfoRefID & 0x00FFFFFF;
		EnsureStateLockInitialized();
		ScopedLock lock(&g_dtfStateLock);
		auto it = DialogueTextFilter::g_spokenGreets.find(speakerRefID);
		if (it != DialogueTextFilter::g_spokenGreets.end() && it->second == activeBaseFormID) {
			Log("DTF: greet STALE topicInfo=0x%08X (active greet=0x%08X already spoken) speaker=0x%08X",
				topicInfoRefID, activeInfoRefID, speakerRefID);
			return false;
		}
		Log("DTF: SKIP false positive topicInfo=0x%08X (active greet=0x%08X) speaker=0x%08X",
			topicInfoRefID, activeInfoRefID, speakerRefID);
		return true;
	}

	Log("DTF: greet validation PASS topicInfo=0x%08X == active=0x%08X speaker=0x%08X",
		topicInfoRefID, activeInfoRefID, speakerRefID);
	return false;
}

static void __cdecl HookCallback(TESTopicInfo* topicInfo, Actor* speaker) {
	if (DialogueTextFilter::g_suppressed) return;
	if (!topicInfo || !speaker)
		return;

	UInt32 speakerRefID = ReadRefID(speaker);
	UInt32 topicInfoRefID = ReadRefID(topicInfo);

	Log("DTF: RunResult fired for topicInfo=0x%08X speaker=0x%08X", topicInfoRefID, speakerRefID);

	if (IsGreetingFalsePositive(speaker, topicInfoRefID, speakerRefID))
		return;

	TESTopicInfoResponse** ppResponse = ThisCall<TESTopicInfoResponse**>(
		kAddr_GetResponses, topicInfo, nullptr);
	if (!ppResponse || !*ppResponse) {
		Log("DTF: no responses for topicInfo=0x%08X, skipping", topicInfoRefID);
		return;
	}

	//strlen * fNoticeTextTimePerCharacter (setting at 0x11D2178, float at +0x04)
	float timePerChar = *(float*)(0x11D2178 + 0x04);
	if (timePerChar <= 0.0f) timePerChar = 0.08f;
	UInt32 topicRefID = ReadRefID(*(void**)((UInt8*)topicInfo + 0x50));
	DWORD queueStartTick = GetTickCount();
	UInt8 fallbackResponseNum = 1;
	UInt32 queuedCount = 0;

	EnsureStateLockInitialized();
	ScopedLock lock(&g_dtfStateLock);
	constexpr size_t kMaxQueuedDialogueEvents = 256;

	for (TESTopicInfoResponse* response = *ppResponse; response; response = response->next, ++fallbackResponseNum) {
		const char* text = response->responseText.CStr();
		size_t textLen = text ? strlen(text) : 0;
		float duration = (float)textLen * timePerChar;
		if (duration < 2.0f) duration = 2.0f;

		if (text && *text) {
			if (DialogueTextFilter::g_pendingEvents.size() >= kMaxQueuedDialogueEvents)
				break;

			UInt8 responseNum = *(UInt8*)((UInt8*)response + 0x0C);
			if (responseNum == 0) responseNum = fallbackResponseNum;

			bool isDuplicate = false;
			for (const auto& existing : DialogueTextFilter::g_pendingEvents) {
				if (existing.speakerRefID == speakerRefID &&
					existing.topicInfoRefID == topicInfoRefID &&
					existing.responseNum == responseNum) {
					isDuplicate = true;
					break;
				}
			}
			if (isDuplicate) continue;

			QueuedDialogueEvent evt{};
			evt.speakerRefID = speakerRefID;
			evt.topicInfoRefID = topicInfoRefID;
			evt.topicRefID = topicRefID;
			evt.dispatchAfterTick = queueStartTick;
			evt.responseNum = responseNum;
			evt.duration = duration;
			strncpy_s(evt.text, sizeof(evt.text), text, _TRUNCATE);
			DialogueTextFilter::g_pendingEvents.push_back(evt);
			++queuedCount;

			Log("DTF: queued resp#%u text=\"%.60s\" dur=%.1f topicInfo=0x%08X speaker=0x%08X",
				responseNum, text, duration, topicInfoRefID, speakerRefID);
		}
	}

	Log("DTF: queued %u total lines for topicInfo=0x%08X speaker=0x%08X", queuedCount, topicInfoRefID, speakerRefID);
}

static auto g_hookCallback = &HookCallback;
static UInt32 g_chainAddr = 0;

static __declspec(naked) void DialogueTextHook() {
	__asm {
		cmp     dword ptr [esp+4], 0
		jnz     skip_filter

		pushad
		pushfd

		//speaker at [esp+8] before pushad(0x20)+pushfd(0x4)
		push    dword ptr [esp+0x2C]
		push    ecx
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

//SpeakSoundFunction hook - commit-level confirmation that a line is actually playing
//0x8A20D0: push ebp(1) + mov ebp,esp(2) + push -1(2) = 5 bytes
constexpr UInt32 kAddr_SpeakSound = 0x8A20D0;
constexpr UInt32 kAddr_SpeakSoundBody = 0x8A20D5;
static UInt32 g_speakChainAddr = 0;

static void __cdecl OnSpeakConfirm(Actor* speaker, const char* voicePath) {
	if (!speaker || !voicePath || !*voicePath) return;

	UInt32 speakerRefID = ReadRefID(speaker);
	if (!speakerRefID) return;

	UInt32 baseFormID = 0;
	UInt8 respNum = 0;
	if (!ParseVoicePathIDs(voicePath, baseFormID, respNum)) return;

	EnsureStateLockInitialized();
	ScopedLock lock(&g_dtfStateLock);

	if (DialogueTextFilter::g_confirmedSpeaks.size() >= 128) return;

	//dedup: engine fires SPEAK twice per line
	DWORD now = GetTickCount();
	for (const auto& rs : DialogueTextFilter::g_recentSpeaks) {
		if (rs.speakerRefID == speakerRefID &&
			rs.baseFormID == baseFormID &&
			rs.responseNum == respNum &&
			(now - rs.timestamp) < 2000) {
			Log("DTF: SPEAK dedup speaker=0x%08X formID=0x%06X resp#%u",
				speakerRefID, baseFormID, respNum);
			return;
		}
	}
	DialogueTextFilter::g_recentSpeaks.push_back({speakerRefID, baseFormID, respNum, now});
	if (DialogueTextFilter::g_recentSpeaks.size() > 64)
		DialogueTextFilter::g_recentSpeaks.erase(DialogueTextFilter::g_recentSpeaks.begin());

	ConfirmedSpeak cs = {};
	cs.speakerRefID = speakerRefID;
	cs.baseFormID = baseFormID;
	cs.responseNum = respNum;
	cs.timestamp = GetTickCount();
	ParseModNameFromVoicePath(voicePath, cs.modName, sizeof(cs.modName));
	strncpy_s(cs.voicePath, sizeof(cs.voicePath), voicePath, _TRUNCATE);
	DialogueTextFilter::g_confirmedSpeaks.push_back(cs);
	DialogueTextFilter::g_spokenGreets[speakerRefID] = baseFormID;

	Log("DTF: SPEAK confirmed speaker=0x%08X formID=0x%06X resp#%u path=\"%.80s\"",
		speakerRefID, baseFormID, respNum, voicePath);
}

static auto g_speakCallback = &OnSpeakConfirm;

static __declspec(naked) void SpeakSoundHook() {
	__asm {
		pushad
		pushfd

		//voicePath at [esp+4] before pushad(0x20)+pushfd(0x04) = [esp+0x28]
		push    dword ptr [esp+0x28]
		push    ecx
		call    [g_speakCallback]
		add     esp, 8

		popfd
		popad

		mov     eax, [g_speakChainAddr]
		test    eax, eax
		jnz     chain_speak

		push    ebp
		mov     ebp, esp
		push    0FFFFFFFFh
		mov     eax, kAddr_SpeakSoundBody
		jmp     eax

	chain_speak:
		jmp     eax
	}
}

namespace DialogueTextFilter {
void Update()
{
	EnsureStateLockInitialized();
	if (g_dtfStateLockInit != 2) return;
	if (!g_eventManagerInterface) return;

	DWORD currentThreadId = GetCurrentThreadId();
	if (!DialogueTextFilter::g_mainThreadId)
		DialogueTextFilter::g_mainThreadId = currentThreadId;
	if (currentThreadId != DialogueTextFilter::g_mainThreadId)
		return;

	//prune old dedup entries (>30s)
	{
		ScopedLock lock(&g_dtfStateLock);
		DWORD now = GetTickCount();
		for (auto it = DialogueTextFilter::g_recentSpeaks.begin();
			 it != DialogueTextFilter::g_recentSpeaks.end(); ) {
			if (now - it->timestamp > 10000)
				it = DialogueTextFilter::g_recentSpeaks.erase(it);
			else
				++it;
		}
		for (auto it = DialogueTextFilter::g_recentFallbacks.begin();
			 it != DialogueTextFilter::g_recentFallbacks.end(); ) {
			if (now - it->timestamp > 30000)
				it = DialogueTextFilter::g_recentFallbacks.erase(it);
			else
				++it;
		}
	}

	std::vector<QueuedDialogueEvent> pendingEvents;
	std::vector<ConfirmedSpeak> confirmedSpeaks;
	{
		ScopedLock lock(&g_dtfStateLock);
		pendingEvents.swap(DialogueTextFilter::g_pendingEvents);
		confirmedSpeaks.swap(DialogueTextFilter::g_confirmedSpeaks);
	}

	DWORD nowTick = GetTickCount();
	std::vector<QueuedDialogueEvent> deferredEvents;
	deferredEvents.reserve(pendingEvents.size());

	for (const auto& evt : pendingEvents) {
		if ((LONG)(nowTick - evt.dispatchAfterTick) < 0) {
			deferredEvents.push_back(evt);
			continue;
		}

		Actor* speaker = reinterpret_cast<Actor*>(Engine::LookupFormByID(evt.speakerRefID));
		TESTopicInfo* topicInfo = reinterpret_cast<TESTopicInfo*>(Engine::LookupFormByID(evt.topicInfoRefID));
		TESTopic* topic = reinterpret_cast<TESTopic*>(Engine::LookupFormByID(evt.topicRefID));

		if (!speaker || !topicInfo || !topic) {
			Log("DTF: DROP resp#%u topicInfo=0x%08X speaker=0x%08X (form lookup failed)",
				evt.responseNum, evt.topicInfoRefID, evt.speakerRefID);
			continue;
		}

		char voicePath[512] = {0};
		bool hasVoice = BuildVoicePath(voicePath, sizeof(voicePath), topicInfo, speaker, evt.responseNum);

		if (!hasVoice) {
			//narrator or unvoiced line - dispatch without confirmation
			Log("DTF: DISPATCH (narrator) resp#%u topicInfo=0x%08X speaker=0x%08X text=\"%.80s\"",
				evt.responseNum, evt.topicInfoRefID, evt.speakerRefID, evt.text);
			g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
				reinterpret_cast<TESObjectREFR*>(speaker),
				speaker, topic, topicInfo, evt.text, "");
			continue;
		}

		//voiced line - require SpeakSoundFunction confirmation
		UInt32 baseFormID = evt.topicInfoRefID & 0x00FFFFFF;
		bool confirmed = false;
		for (auto cit = confirmedSpeaks.begin(); cit != confirmedSpeaks.end(); ++cit) {
			if (cit->speakerRefID == evt.speakerRefID &&
				cit->baseFormID == baseFormID &&
				cit->responseNum == evt.responseNum) {
				confirmed = true;
				confirmedSpeaks.erase(cit);
				break;
			}
		}

		if (confirmed) {
			//skip if already fallback-dispatched (post-reload re-evaluation)
			bool alreadyFallback = false;
			for (auto it = DialogueTextFilter::g_recentFallbacks.begin();
				 it != DialogueTextFilter::g_recentFallbacks.end(); ++it) {
				if (it->speakerRefID == evt.speakerRefID &&
					it->baseFormID == baseFormID &&
					it->responseNum == evt.responseNum) {
					alreadyFallback = true;
					DialogueTextFilter::g_recentFallbacks.erase(it);
					break;
				}
			}
			if (alreadyFallback) {
				Log("DTF: SKIP (already fallback-dispatched) resp#%u topicInfo=0x%08X speaker=0x%08X",
					evt.responseNum, evt.topicInfoRefID, evt.speakerRefID);
				continue;
			}
			Log("DTF: DISPATCH resp#%u topicInfo=0x%08X speaker=0x%08X voice=\"%s\" text=\"%.80s\"",
				evt.responseNum, evt.topicInfoRefID, evt.speakerRefID, voicePath, evt.text);
			g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
				reinterpret_cast<TESObjectREFR*>(speaker),
				speaker, topic, topicInfo, evt.text, voicePath);
		} else if (nowTick - evt.dispatchAfterTick > 30000) {
			Log("DTF: TIMEOUT resp#%u topicInfo=0x%08X speaker=0x%08X (no confirmation after 30s)",
				evt.responseNum, evt.topicInfoRefID, evt.speakerRefID);
		} else {
			deferredEvents.push_back(evt);
		}
	}

	//fallback: SPEAK with no RunResult (fire-and-forget barks after reload etc)
	for (auto cit = confirmedSpeaks.begin(); cit != confirmedSpeaks.end(); ) {
		if (nowTick - cit->timestamp < 500) { ++cit; continue; }

		UInt8 modIndex = FindModIndex(cit->modName);
		if (modIndex == 0xFF) {
			Log("DTF: FALLBACK DROP speaker=0x%08X formID=0x%06X (mod '%s' not found)",
				cit->speakerRefID, cit->baseFormID, cit->modName);
			cit = confirmedSpeaks.erase(cit);
			continue;
		}

		UInt32 fullFormID = ((UInt32)modIndex << 24) | cit->baseFormID;
		TESTopicInfo* info = reinterpret_cast<TESTopicInfo*>(Engine::LookupFormByID(fullFormID));
		Actor* speaker = reinterpret_cast<Actor*>(Engine::LookupFormByID(cit->speakerRefID));
		if (!info || !speaker) {
			Log("DTF: FALLBACK DROP speaker=0x%08X formID=0x%08X (lookup failed)",
				cit->speakerRefID, fullFormID);
			cit = confirmedSpeaks.erase(cit);
			continue;
		}

		TESTopicInfoResponse** ppResp = ThisCall<TESTopicInfoResponse**>(
			kAddr_GetResponses, info, nullptr);
		const char* text = nullptr;
		if (ppResp && *ppResp) {
			UInt8 rn = 1;
			for (auto* r = *ppResp; r; r = r->next, rn++) {
				if (rn == cit->responseNum) { text = r->responseText.CStr(); break; }
			}
		}

		if (!text || !*text) {
			Log("DTF: FALLBACK DROP speaker=0x%08X formID=0x%08X resp#%u (no text)",
				cit->speakerRefID, fullFormID, cit->responseNum);
			cit = confirmedSpeaks.erase(cit);
			continue;
		}

		TESTopic* topic = *reinterpret_cast<TESTopic**>((UInt8*)info + 0x50);

		Log("DTF: FALLBACK DISPATCH speaker=0x%08X formID=0x%08X resp#%u text=\"%.80s\"",
			cit->speakerRefID, fullFormID, cit->responseNum, text);
		g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
			reinterpret_cast<TESObjectREFR*>(speaker),
			speaker, topic, info, text, cit->voicePath);
		DialogueTextFilter::g_recentFallbacks.push_back(
			{cit->speakerRefID, cit->baseFormID, cit->responseNum, nowTick});

		cit = confirmedSpeaks.erase(cit);
	}

	{
		ScopedLock lock(&g_dtfStateLock);
		if (!deferredEvents.empty()) {
			DialogueTextFilter::g_pendingEvents.insert(
				DialogueTextFilter::g_pendingEvents.begin(),
				deferredEvents.begin(), deferredEvents.end());

			constexpr size_t kMaxQueuedDialogueEvents = 256;
			if (DialogueTextFilter::g_pendingEvents.size() > kMaxQueuedDialogueEvents)
				DialogueTextFilter::g_pendingEvents.resize(kMaxQueuedDialogueEvents);
		}
		//preserve unconsumed confirmations for next frame
		if (!confirmedSpeaks.empty()) {
			DialogueTextFilter::g_confirmedSpeaks.insert(
				DialogueTextFilter::g_confirmedSpeaks.end(),
				confirmedSpeaks.begin(), confirmedSpeaks.end());
			if (DialogueTextFilter::g_confirmedSpeaks.size() > 128)
				DialogueTextFilter::g_confirmedSpeaks.erase(
					DialogueTextFilter::g_confirmedSpeaks.begin(),
					DialogueTextFilter::g_confirmedSpeaks.begin() +
						(DialogueTextFilter::g_confirmedSpeaks.size() - 128));
		}
	}
}

bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	EnsureStateLockInitialized();
	DialogueTextFilter::g_mainThreadId = 0;

	//RunResult hook - queues candidate events
	UInt8 firstByte = *(UInt8*)kAddr_RunResult;
	if (firstByte == 0xE9)
		g_chainAddr = SafeWrite::GetRelJumpTarget(kAddr_RunResult);
	else
		g_chainAddr = 0;

	SafeWrite::WriteRelJump(kAddr_RunResult, (UInt32)DialogueTextHook);
	SafeWrite::Write8(kAddr_RunResult + 5, 0x90);

	//SpeakSoundFunction hook - confirms which line actually plays
	UInt8 speakFirstByte = *(UInt8*)kAddr_SpeakSound;
	if (speakFirstByte == 0xE9)
		g_speakChainAddr = SafeWrite::GetRelJumpTarget(kAddr_SpeakSound);
	else
		g_speakChainAddr = 0;

	SafeWrite::WriteRelJump(kAddr_SpeakSound, (UInt32)SpeakSoundHook);
	DialogueTextFilter::g_hookInstalled = true;

	return true;
}
}
