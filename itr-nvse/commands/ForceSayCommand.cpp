//ForceSay - makes an actor say a topic line, dead or alive
//picks the line like Say does, forces the first line when nothing matches, borrows another voice type when the speaker's is missing

#include "ForceSayCommand.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include <cstring>
#include <cstdio>
#include <windows.h>

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

struct BSSoundHandle
{
	UInt32 uiSoundID;
	bool bAssumeSuccess;
	UInt32 uiState;
	BSSoundHandle() : uiSoundID(0xFFFFFFFF), bAssumeSuccess(false), uiState(0) {}
};

struct BSHash { UInt8 pad[8]; };

static TESTopic::Info* GetFirstTopicInfo(TESTopic* topic)
{
	auto* head = reinterpret_cast<TESTopicInfoListNodeView*>(&topic->infos);
	return head->item;
}

//check if a voice file exists in loaded BSA archives
//uses ArchiveManager hash lookup - no audio side effects
static bool ExistsInBSA(const char* path)
{
	//BSA paths don't include "Data\" prefix - strip it if present
	const char* bsaPath = path;
	if (_strnicmp(bsaPath, "Data\\", 5) == 0)
		bsaPath += 5;

	BSHash dirHash, fileHash;
	CdeclCall(0xAFD270, bsaPath, &dirHash, &fileHash); //BSHash::MakeDirAndFile
	//type 4 = Voices (bit 4 = 0x10)
	void* entry = CdeclCall<void*>(0xAF6540, 4, &dirHash, &fileHash, bsaPath);
	if (entry) return true;
	//also try type 3 = Sounds (bit 3 = 0x08)
	entry = CdeclCall<void*>(0xAF6540, 3, &dirHash, &fileHash, bsaPath);
	return entry != nullptr;
}

static bool VoiceFileExists(const char* path)
{
	DWORD attr = GetFileAttributesA(path);
	if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
		return true;
	return ExistsInBSA(path);
}

//voice path: Data\Sound\Voice\<plugin>\<voice type edid>\<filename> (0x616FA0)
//when the speaker's voice type never recorded the line, borrow any voice type that did
static bool ResolveVoicePath(const char* voicePath, char* out, UInt32 outSize)
{
	snprintf(out, outSize, "%s", voicePath);
	//the loader opens .ogg whatever the extension (0xAF1E00)
	char* ext = strrchr(out, '.');
	if (ext && strlen(ext) == 4)
		memcpy(ext + 1, "ogg", 3);
	if (VoiceFileExists(out))
		return true;

	int slashCount = 0;
	const char* seg4start = nullptr;
	const char* seg5 = nullptr;
	for (const char* p = voicePath; *p; p++)
	{
		if (*p == '\\')
		{
			slashCount++;
			if (slashCount == 4) seg4start = p + 1;
			if (slashCount == 5) { seg5 = p; break; }
		}
	}
	if (!seg4start || !seg5)
		return false;

	char filename[260];
	snprintf(filename, sizeof(filename), "%s", out + (seg5 - voicePath)); //includes leading backslash
	int prefixLen = (int)(seg4start - voicePath);

	auto* dataHandler = static_cast<DataHandler*>(*g_dataHandlerPtr);
	if (!dataHandler)
		return false;
	for (auto iter = dataHandler->voiceTypeList.Begin(); !iter.End(); ++iter)
	{
		const char* edid = Engine::TESForm_GetEditorID(iter.Get());
		if (!edid || !*edid)
			continue;
		snprintf(out, outSize, "%.*s%s%s", prefixLen, voicePath, edid, filename);
		if (VoiceFileExists(out))
		{
			Log("ForceSay: borrowed voice %s for %s", edid, voicePath);
			return true;
		}
	}
	return false;
}

static ParamInfo kParams_ForceSay[] = {
	{"topic", kParamType_Topic, 0},
	{"target", kParamType_Actor, 1},
};

DEFINE_COMMAND_PLUGIN(ForceSay, "Force an actor to say a topic line, dead or alive", 1, 2, kParams_ForceSay)

namespace ForceSayCommand
{
bool SayTopic(Actor* speaker, TESTopic* topic, Actor* target)
{
	if (!speaker || !topic)
		return false;

	if (speaker->typeID != kFormType_ACHR && speaker->typeID != kFormType_ACRE)
		return false;

	if (!Engine::Actor_GetProcess(speaker) || !speaker->GetNiNode())
		return false;

	BSSoundHandle soundHandle;
	using VoiceSoundFunctionEx_t = BSSoundHandle*(__thiscall*)(Actor*, BSSoundHandle*, TESTopic*, Actor*, bool, bool, UInt32, bool);
	auto VoiceSoundFunctionEx = reinterpret_cast<VoiceSoundFunctionEx_t>(0x8A1BD0);
	VoiceSoundFunctionEx(speaker, &soundHandle, topic, target, false, false, 0, true);
	return soundHandle.uiSoundID != 0xFFFFFFFF;
}

bool ForceSay(Actor* speaker, TESTopic* topic, Actor* target)
{
	if (!speaker || !topic)
		return false;

	if (speaker->typeID != kFormType_ACHR && speaker->typeID != kFormType_ACRE)
		return false;

	void* process = Engine::Actor_GetProcess(speaker);
	if (!process)
		return false;
	if (!speaker->GetNiNode())
		return false;

	//the line pick Say uses via 0x8A1BD0 (quest running, conditions, say once, random), it has no death check
	auto* item = ThisCall<DialogueItem*>(0x61B320, topic, speaker, target, 0, 0, 0);
	if (!item)
	{
		//nothing matched, force the first line ignoring conditions
		TESTopic::Info* firstInfo = GetFirstTopicInfo(topic);
		if (!firstInfo || firstInfo->infoArray.numObjs == 0)
			return false;

		TESTopicInfo* firstTopicInfo = firstInfo->infoArray.data[0];
		TESQuest* quest = firstInfo->quest;
		if (!firstTopicInfo || !quest)
			return false;

		void* mem = Engine::GameHeapAlloc(sizeof(DialogueItem));
		if (!mem)
			return false;
		item = ThisCall<DialogueItem*>(0x83C520, mem, quest, topic, firstTopicInfo, speaker); //DialogueItem_0
	}
	TESTopicInfo* topicInfo = item->currentTopicInfo;

	//max lip distance so lip sync works at any range
	UInt32* lipDist = GetVoiceLipDistanceLimit();
	UInt32 oldLipDist = *lipDist;
	*lipDist = 0x7FFFFFFF;

	ThisCall<bool>(0x83C7B0, item); //FirstResponse
	DialogueResponse* response = ThisCall<DialogueResponse*>(0x83C820, item); //GetCurrentItem

	if (!response)
	{
		ThisCall(0x83C670, item);
		Engine::GameHeapFree(item);
		*lipDist = oldLipDist;
		return false;
	}

	char* voicePath       = response->voiceFileName.m_data;
	UInt32 emotionType    = response->emotionType;
	UInt32 emotionValue   = static_cast<UInt32>(response->emotionValue);
	UInt16 textLen        = response->responseText.m_dataLen;
	TESIdleForm* speakerAnim = response->speakerIdle;
	TESIdleForm* listenerAnim = response->listenerIdle;

	if (!voicePath || !voicePath[0])
	{
		ThisCall(0x83C670, item);
		Engine::GameHeapFree(item);
		*lipDist = oldLipDist;
		return false;
	}

	//unvoiced lines keep the original path, like Say they play as a silent subtitle and run their result script
	char finalPath[512];
	if (!ResolveVoicePath(voicePath, finalPath, sizeof(finalPath)))
		snprintf(finalPath, sizeof(finalPath), "%s", voicePath);

	BSSoundHandle soundHandle;
	double delay = ThisCall<double>(0x8A20D0, speaker,
		finalPath, &soundHandle,
		emotionType, emotionValue, (UInt32)textLen,
		speakerAnim, listenerAnim, target,
		true, false, false, true, true); //abQueue=false for sync lip load

	//0xAD8E60 skips registration for an invalid handle and can run the callback inline,
	//so a refused line (no 3d, severed head or brain) must bail before any sayto bookkeeping
	if (soundHandle.uiSoundID == 0xFFFFFFFF)
	{
		ThisCall(0x83C670, item);
		Engine::GameHeapFree(item);
		*lipDist = oldLipDist;
		return false;
	}

	ThisCall(0x57AD20, speaker, topic); //SetSayToTopic
	ThisCall(0x57ACE0, speaker, topicInfo); //SetSayToTopicInfo
	ThisCall(0x57AD60, speaker, 1); //SetSayToResponseNumber

	ThisCall(0xAD8E60, &soundHandle,
		(void*)0x936A20, //Actor::SayToCallBack
		(void*)(speaker->refID));

	//SetDoingSayTo via process vtbl+0x88, highprocess 0x8D8DC0 writes this+0x459,
	//middlehigh override 0x4534F0 is a no-op stub, a direct highprocess call would write oob on smaller processes
	UInt32 fnSetDoingSayTo = (*(UInt32**)process)[0x88 / 4];
	ThisCall(fnSetDoingSayTo, process, (UInt8)1);

	//restore before the result script, 0x83C850 runs it synchronously and a re-entering
	//forcesay would capture the inflated value as its saved original
	*lipDist = oldLipDist;

	ThisCall(0x83C850, item, 0); //RunResult(TIRS_BEGIN)

	ThisCall(0x83C670, item);
	Engine::GameHeapFree(item);

	return true;
}
}

bool Cmd_ForceSay_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESTopic* topic = nullptr;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &topic, &target))
		return true;

	if (thisObj && ForceSayCommand::ForceSay((Actor*)thisObj, topic, target))
		*result = 1;

	return true;
}

namespace ForceSayCommand {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceSay);
}
}
