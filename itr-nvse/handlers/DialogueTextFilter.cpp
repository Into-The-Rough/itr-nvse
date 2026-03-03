//dialogue text filter - hooks dialogue responses and dispatches events

#include <vector>
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

namespace DialogueTextFilter {
	std::vector<QueuedDialogueEvent> g_pendingEvents;
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

static const char* GetModName(UInt8 modIndex) {
	void* dh = *(void**)0x11C3F2C;
	if (!dh || modIndex >= 0xFF) return nullptr;
	ModInfo* modInfo = *(ModInfo**)((UInt8*)dh + 0x21C + (modIndex * 4));
	if (!modInfo) return nullptr;
	return (const char*)((UInt8*)modInfo + 0x20);
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

	sprintf_s(outPath, outSize,
	          "Data\\Sound\\Voice\\%s\\%s\\%s_%s_%08X_%u.ogg",
	          modName, voiceTypeID, questID, topicID, baseFormID, responseNum);

	return true;
}

static bool IsValidFormPointer(void* form) {
	if (!form) return false;
	//SEH required: called from AI thread where form pointers can be dangling
	__try {
		UInt32 refID = *(UInt32*)((UInt8*)form + 0x0C);
		UInt32 flags = *(UInt32*)((UInt8*)form + 0x08);
		if (refID == 0) return false;
		if (flags & 0x20) return false;  //kFormFlags_Deleted
		if (flags & 0x800) return false; //kFormFlags_Temporary
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void DTF_Suppress(bool suppress) {
	DialogueTextFilter::g_suppressed = suppress;
}

static void __cdecl HookCallback(TESTopicInfo* topicInfo, Actor* speaker) {
	if (DialogueTextFilter::g_suppressed) return;
	if (!IsValidFormPointer(topicInfo) || !IsValidFormPointer(speaker))
		return;

	TESTopicInfoResponse** ppResponse = ThisCall<TESTopicInfoResponse**>(
		kAddr_GetResponses, topicInfo, nullptr);
	if (!ppResponse || !*ppResponse)
		return;

	//strlen * fNoticeTextTimePerCharacter (setting at 0x11D2178, float at +0x04)
	float timePerChar = *(float*)(0x11D2178 + 0x04);
	if (timePerChar <= 0.0f) timePerChar = 0.08f;
	UInt32 speakerRefID = ReadRefID(speaker);
	UInt32 topicInfoRefID = ReadRefID(topicInfo);
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

	if (queuedCount > 1) {
		Log("DTF: queued %u lines for topicInfo=0x%08X speaker=0x%08X", queuedCount, topicInfoRefID, speakerRefID);
	}
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

void DTF_Update()
{
	EnsureStateLockInitialized();
	if (g_dtfStateLockInit != 2) return;
	if (!g_eventManagerInterface) return;

	DWORD currentThreadId = GetCurrentThreadId();
	if (!DialogueTextFilter::g_mainThreadId)
		DialogueTextFilter::g_mainThreadId = currentThreadId;
	if (currentThreadId != DialogueTextFilter::g_mainThreadId)
		return;

	std::vector<QueuedDialogueEvent> pendingEvents;
	{
		ScopedLock lock(&g_dtfStateLock);
		pendingEvents.swap(DialogueTextFilter::g_pendingEvents);
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

		if (speaker && topicInfo && topic) {
			char voicePath[512] = {0};
			if (!BuildVoicePath(voicePath, sizeof(voicePath), topicInfo, speaker, evt.responseNum))
				voicePath[0] = '\0';
			if (evt.responseNum > 1) {
				Log("DTF: dispatch response %u for topicInfo=0x%08X text=\"%.64s\"", evt.responseNum, evt.topicInfoRefID, evt.text);
			}
			g_eventManagerInterface->DispatchEvent("ITR:OnDialogueText",
				reinterpret_cast<TESObjectREFR*>(speaker),
				speaker, topic, topicInfo, evt.text, voicePath);
		}
	}

	if (!deferredEvents.empty()) {
		ScopedLock lock(&g_dtfStateLock);
		DialogueTextFilter::g_pendingEvents.insert(
			DialogueTextFilter::g_pendingEvents.begin(),
			deferredEvents.begin(), deferredEvents.end());

		constexpr size_t kMaxQueuedDialogueEvents = 256;
		if (DialogueTextFilter::g_pendingEvents.size() > kMaxQueuedDialogueEvents)
			DialogueTextFilter::g_pendingEvents.resize(kMaxQueuedDialogueEvents);
	}
}

bool DTF_Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	EnsureStateLockInitialized();
	DialogueTextFilter::g_mainThreadId = 0;

	//install hook unconditionally for EventManager dispatch
	UInt8 firstByte = *(UInt8*)kAddr_RunResult;
	if (firstByte == 0xE9)
		g_chainAddr = SafeWrite::GetRelJumpTarget(kAddr_RunResult);
	else
		g_chainAddr = 0;

	SafeWrite::WriteRelJump(kAddr_RunResult, (UInt32)DialogueTextHook);
	SafeWrite::Write8(kAddr_RunResult + 5, 0x90);
	DialogueTextFilter::g_hookInstalled = true;

	return true;
}
