//dialogue text filter - hooks dialogue responses and dispatches events

#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <Windows.h>

#include "DialogueTextFilter.h"
#define ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/NVSEMinimal.h"
#undef ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/EventDispatch.h"
#include "internal/globals.h"
#include "internal/GameLayout.h"

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
	float durationSeconds;
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
constexpr UInt32 kAddr_GetResponses = 0x61E780;

static UInt32 ReadRefID(const TESForm* form)
{
	return form ? form->refID : 0;
}

static const char* StringCStr(const String& str)
{
	return str.m_data ? str.m_data : "";
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
	auto* dh = reinterpret_cast<DataHandler*>(*g_dataHandlerPtr);
	if (!dh || modIndex >= 0xFF) return nullptr;
	ModInfo* modInfo = dh->modList.loadedMods[modIndex];
	if (!modInfo) return nullptr;
	return modInfo->name;
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

static BGSVoiceType* GetActorVoiceType(Actor* actor) {
	if (!actor) return nullptr;
	TESForm* baseForm = actor->baseForm;
	if (!baseForm) return nullptr;

	if (baseForm->typeID != kFormType_NPC) return nullptr;

	auto* npc = static_cast<TESNPC*>(baseForm);
	BGSVoiceType* voiceType = npc->baseData.voiceType;
	if (voiceType && voiceType->typeID == kFormType_VoiceType) return voiceType;

	voiceType = TESActorBaseGetLegacyVoiceTypeFallback(baseForm);
	if (voiceType && voiceType->typeID == kFormType_VoiceType) return voiceType;

	return nullptr;
}

static const char* GetVoiceTypeEditorID(BGSVoiceType* voiceType) {
	if (!voiceType) return nullptr;
	auto* view = reinterpret_cast<BGSVoiceTypeEditorIDView*>(voiceType);
	return StringCStr(view->editorID);
}

static TESTopic* GetTopicInfoParentTopic(TESTopicInfo* topicInfo)
{
	return TESTopicInfoGetParentTopic(topicInfo);
}

static bool BuildVoicePath(char* outPath, size_t outSize,
	                          TESTopicInfo* topicInfo, TESTopic* topic, Actor* speaker,
	                          UInt8 responseNum)
{
	if (!outPath || !topicInfo || !speaker) return false;
	outPath[0] = '\0';

	UInt32 formID = topicInfo->refID;
	UInt8 modIndex = (UInt8)(formID >> 24);
	UInt32 baseFormID = formID & 0x00FFFFFF;

	const char* modName = GetModName(modIndex);
	if (!modName || !*modName) return false;

	BGSVoiceType* voiceType = GetActorVoiceType(speaker);
	const char* voiceTypeID = voiceType ? GetVoiceTypeEditorID(voiceType) : nullptr;
	if (!voiceTypeID || !*voiceTypeID) return false;

	if (!topic)
		topic = GetTopicInfoParentTopic(topicInfo);

	TESQuest* quest = TESTopicGetQuestForInfo(topic, topicInfo);
	const char* questID = Engine::TESForm_GetEditorID(quest);
	if (!questID) questID = "";

	const char* topicID = Engine::TESForm_GetEditorID(topic);
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

//RunResult can fire for script-chain side effects during greeting evaluation.
//Ignore topicInfo mismatches unless SpeakSound already confirmed the line.
static bool IsGreetingFalsePositive(Actor* speaker, UInt32 topicInfoRefID, UInt32 speakerRefID) {
	//talking activators reach this hook too - skip non-actors before walking MobileObject
	TESForm* baseForm = speaker->baseForm;
	if (!baseForm) return false;
	UInt8 baseType = baseForm->typeID;
	if (baseType != kFormType_NPC && baseType != kFormType_Creature) return false;

	BaseProcess* process = speaker->baseProcess;
	if (!process) return false;

	if (process->processLevel != 0) return false;

	auto* dialogueItem = reinterpret_cast<DialogueItem*>(HighProcessGetQueuedGreetTopic(process));
	if (!dialogueItem) {
		return false;
	}

	UInt32 activeInfoRefID = ReadRefID(dialogueItem->currentTopicInfo);

	if (activeInfoRefID && activeInfoRefID != topicInfoRefID) {
		//A spoken queued greet means pQueuedGreetTopic is stale, not conflicting.
		UInt32 activeBaseFormID = activeInfoRefID & 0x00FFFFFF;
		EnsureStateLockInitialized();
		ScopedLock lock(&g_dtfStateLock);
		auto it = DialogueTextFilter::g_spokenGreets.find(speakerRefID);
		if (it != DialogueTextFilter::g_spokenGreets.end() && it->second == activeBaseFormID) {
			return false;
		}
		return true;
	}

	return false;
}

static bool IsActorRef(TESForm* form) {
	if (!form) return false;
	return form->typeID == kFormType_ACHR || form->typeID == kFormType_ACRE;
}

static bool IsWorldRef(TESForm* form) {
	if (!form) return false;
	return form->typeID >= kFormType_Reference && form->typeID <= kFormType_ACRE;
}

static void __cdecl HookCallback(TESTopicInfo* topicInfo, Actor* speaker) {
	if (DialogueTextFilter::g_suppressed) return;
	if (!topicInfo || !speaker)
		return;
	if (!IsWorldRef(speaker)) return;

	UInt32 speakerRefID = ReadRefID(speaker);
	UInt32 topicInfoRefID = ReadRefID(topicInfo);

	if (IsActorRef(speaker) && IsGreetingFalsePositive(speaker, topicInfoRefID, speakerRefID))
		return;

	TESTopicInfoResponse** ppResponse = ThisCall<TESTopicInfoResponse**>(
		kAddr_GetResponses, topicInfo, nullptr);
	if (!ppResponse || !*ppResponse) {
		return;
	}

	float timePerChar = Engine::GetSettingFloatValue(g_fDialogueTextMinSecondsPerCharSetting, 0.08f);
	if (timePerChar <= 0.0f) timePerChar = 0.08f;
	UInt32 topicRefID = ReadRefID(GetTopicInfoParentTopic(topicInfo));
	DWORD queueStartTick = GetTickCount();
	UInt8 fallbackResponseNum = 1;
	UInt32 queuedCount = 0;

	EnsureStateLockInitialized();
	ScopedLock lock(&g_dtfStateLock);
	constexpr size_t kMaxQueuedDialogueEvents = 256;

	for (TESTopicInfoResponse* response = *ppResponse; response; response = response->next, ++fallbackResponseNum) {
		const char* text = StringCStr(response->responseText);
		size_t textLen = text ? strlen(text) : 0;
		float duration = (float)textLen * timePerChar;
		if (duration < 2.0f) duration = 2.0f;

		if (text && *text) {
			if (DialogueTextFilter::g_pendingEvents.size() >= kMaxQueuedDialogueEvents)
				break;

			UInt8 responseNum = response->data.responseNumber;
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
		}
	}

}

using RunResult_t = void(__thiscall*)(TESTopicInfo*, int, Actor*);
static Detours::JumpDetour g_runResultDetour;
static RunResult_t g_runResult = nullptr;

static void __fastcall DialogueTextHook(TESTopicInfo* topicInfo, void*, int responseIndex, Actor* speaker) {
	if (responseIndex == 0)
		HookCallback(topicInfo, speaker);

	if (g_runResult)
		g_runResult(topicInfo, responseIndex, speaker);
}

//SpeakSound computes the final engine duration immediately before returning it.
constexpr UInt32 kAddr_SpeakSoundDuration = 0x8A2CD3;

static void __cdecl OnSpeakConfirm(Actor* speaker, const char* voicePath, float durationSeconds) {
	if (g_isLoadingSave) return;
	if (!speaker || !voicePath || !*voicePath) return;
	if (!IsWorldRef(speaker)) return;
	if (!(durationSeconds > 0.0f) || durationSeconds > 3600.0f)
		durationSeconds = 0.0f;

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
	cs.durationSeconds = durationSeconds;
	ParseModNameFromVoicePath(voicePath, cs.modName, sizeof(cs.modName));
	strncpy_s(cs.voicePath, sizeof(cs.voicePath), voicePath, _TRUNCATE);
	DialogueTextFilter::g_confirmedSpeaks.push_back(cs);
	DialogueTextFilter::g_spokenGreets[speakerRefID] = baseFormID;
}

static auto g_speakCallback = &OnSpeakConfirm;
static Detours::JumpDetour g_speakDurationDetour;
static UInt8* g_speakDurationTrampoline = nullptr;

static __declspec(naked) void SpeakSoundDurationHook() {
	__asm {
		pushad
		pushfd

		push    dword ptr [ebp-228h]          //cdecl arg3: durationSeconds
		push    dword ptr [ebp+08h]           //cdecl arg2: voicePath
		push    dword ptr [ebp-298h]          //cdecl arg1: speaker
		call    [g_speakCallback]
		add     esp, 0Ch

		popfd
		popad

		mov     eax, g_speakDurationTrampoline
		jmp     eax
	}
}

namespace DialogueTextFilter {
void ClearState()
{
	EnsureStateLockInitialized();
	ScopedLock lock(&g_dtfStateLock);
	g_pendingEvents.clear();
	g_confirmedSpeaks.clear();
	g_recentSpeaks.clear();
	g_recentFallbacks.clear();
	g_spokenGreets.clear();
	g_mainThreadId = 0;
}

void Update()
{
	EnsureStateLockInitialized();
	if (g_dtfStateLockInit != 2) return;
	if (!g_eventManagerInterface) return;
	if (g_isLoadingSave) {
		ClearState();
		return;
	}

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

		if (!speaker || !topicInfo || !topic || !IsWorldRef(speaker)) {
			continue;
		}

		char voicePath[512] = {0};
		bool hasVoice = IsActorRef(speaker) &&
			BuildVoicePath(voicePath, sizeof(voicePath), topicInfo, topic, speaker, evt.responseNum);

		if (!hasVoice) {
			g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
				reinterpret_cast<TESObjectREFR*>(speaker),
				speaker, topic, topicInfo, evt.text, "", PackEventFloatArg(evt.duration));
			continue;
		}

		UInt32 baseFormID = evt.topicInfoRefID & 0x00FFFFFF;
		bool confirmed = false;
		float durationSeconds = 0.0f;
		char confirmedVoicePath[260] = {0};
		for (auto cit = confirmedSpeaks.begin(); cit != confirmedSpeaks.end(); ++cit) {
			if (cit->speakerRefID == evt.speakerRefID &&
				cit->baseFormID == baseFormID &&
				cit->responseNum == evt.responseNum) {
				confirmed = true;
				durationSeconds = cit->durationSeconds;
				strncpy_s(confirmedVoicePath, sizeof(confirmedVoicePath), cit->voicePath, _TRUNCATE);
				confirmedSpeaks.erase(cit);
				break;
			}
		}

		if (confirmed) {
			bool alreadyFallback = false;
			{
				ScopedLock lock(&g_dtfStateLock);
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
			}
			if (alreadyFallback) {
				continue;
			}
			g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
				reinterpret_cast<TESObjectREFR*>(speaker),
				speaker, topic, topicInfo, evt.text,
				confirmedVoicePath[0] ? confirmedVoicePath : voicePath,
				PackEventFloatArg(durationSeconds > 0.0f ? durationSeconds : evt.duration));
		} else if (nowTick - evt.dispatchAfterTick > 30000) {
			//gave up waiting for speaksound confirm, drop the event
		} else {
			deferredEvents.push_back(evt);
		}
	}

	//Some lines confirm through SpeakSound after reload without a matching RunResult.
	for (auto cit = confirmedSpeaks.begin(); cit != confirmedSpeaks.end(); ) {
		if (nowTick - cit->timestamp < 500) { ++cit; continue; }

		UInt8 modIndex = FindModIndex(cit->modName);
		if (modIndex == 0xFF) {
			cit = confirmedSpeaks.erase(cit);
			continue;
		}

		UInt32 fullFormID = ((UInt32)modIndex << 24) | cit->baseFormID;
		TESTopicInfo* info = reinterpret_cast<TESTopicInfo*>(Engine::LookupFormByID(fullFormID));
		Actor* speaker = reinterpret_cast<Actor*>(Engine::LookupFormByID(cit->speakerRefID));
		if (!info || !speaker || !IsWorldRef(speaker)) {
			cit = confirmedSpeaks.erase(cit);
			continue;
		}

		TESTopicInfoResponse** ppResp = ThisCall<TESTopicInfoResponse**>(
			kAddr_GetResponses, info, nullptr);
		char textBuf[1024] = {0};
		bool gotText = false;
		if (ppResp && *ppResp) {
			UInt8 rn = 1;
			for (auto* r = *ppResp; r; r = r->next, rn++) {
				if (rn == cit->responseNum) {
					const char* src = StringCStr(r->responseText);
					if (src && *src) {
						strncpy_s(textBuf, sizeof(textBuf), src, _TRUNCATE);
						gotText = true;
					}
					break;
				}
			}
		}

		if (!gotText) {
			cit = confirmedSpeaks.erase(cit);
			continue;
		}

		TESTopic* topic = GetTopicInfoParentTopic(info);

		g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
			reinterpret_cast<TESObjectREFR*>(speaker),
			speaker, topic, info, textBuf, cit->voicePath, PackEventFloatArg(cit->durationSeconds));
		{
			ScopedLock lock(&g_dtfStateLock);
			DialogueTextFilter::g_recentFallbacks.push_back(
				{cit->speakerRefID, cit->baseFormID, cit->responseNum, nowTick});
		}

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

	bool runResultHooked = false;
	if (*(UInt8*)kAddr_RunResult == 0xE9)
		runResultHooked = g_runResultDetour.WriteRelJumpChainable(kAddr_RunResult, DialogueTextHook, 5);
	else
		runResultHooked = g_runResultDetour.WriteRelJump(kAddr_RunResult, DialogueTextHook, 6);
	if (!runResultHooked) {
		Log("DialogueTextFilter: failed to install RunResult detour");
		return false;
	}
	g_runResult = g_runResultDetour.GetTrampoline<RunResult_t>();
	if (!g_runResult) {
		Log("DialogueTextFilter: RunResult trampoline missing");
		g_runResultDetour.Remove();
		return false;
	}

	static const UInt8 expectedSpeakDurationBytes[] = { 0xD9, 0x85, 0xD8, 0xFD, 0xFF, 0xFF };
	if (memcmp((void*)kAddr_SpeakSoundDuration, expectedSpeakDurationBytes, sizeof(expectedSpeakDurationBytes)) == 0) {
		if (!g_speakDurationDetour.WriteRelJump(kAddr_SpeakSoundDuration, SpeakSoundDurationHook, sizeof(expectedSpeakDurationBytes), &g_speakDurationTrampoline))
			Log("DialogueTextFilter: failed to install SpeakSound duration detour");
	}
	DialogueTextFilter::g_hookInstalled = true;

	return true;
}
}
