//ForceSay - makes an actor say a topic line ignoring all conditions
//bypasses quest running, quest/info conditions, sayonce, voicetype, isdead

#include "ForceSayCommand.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
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

//voice path: Data\Sound\Voice\<plugin>\<VOICETYPE>\<filename>
//scan sibling voice type directories for one that has the actual file
static bool FixVoicePath(const char* voicePath, char* outFixed, UInt32 outSize)
{
	//BSA is authoritative - if the file exists in BSA at the current path, keep it
	if (ExistsInBSA(voicePath))
		return false;

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

	char currentVT[128];
	int vtLen = (int)(seg5 - seg4start);
	if (vtLen >= 128) vtLen = 127;
	memcpy(currentVT, seg4start, vtLen);
	currentVT[vtLen] = 0;

	char searchDir[512];
	int prefixLen = (int)(seg4start - voicePath);
	snprintf(searchDir, 512, "%.*s*", prefixLen, voicePath);

	const char* filename = seg5; //includes leading backslash

	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA(searchDir, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return false;

	bool found = false;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (fd.cFileName[0] == '.')
			continue;
		if (_stricmp(fd.cFileName, currentVT) == 0)
			continue;

		snprintf(outFixed, outSize, "%.*s%s%s", prefixLen, voicePath, fd.cFileName, filename);

		if (ExistsInBSA(outFixed))
		{
			found = true;
			break;
		}
	} while (FindNextFileA(hFind, &fd));

	FindClose(hFind);
	return found;
}

static ParamInfo kParams_ForceSay[] = {
	{"topic", kParamType_Topic, 0},
	{"target", kParamType_Actor, 1},
};

DEFINE_COMMAND_PLUGIN(ForceSay, "Force an actor to say a topic line, ignoring all conditions", 1, 2, kParams_ForceSay)

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

	//walk topic->infos to get first topicinfo + quest directly (bypass all conditions)
	TESTopic::Info* firstInfo = GetFirstTopicInfo(topic);
	if (!firstInfo)
		return false;

	if (firstInfo->infoArray.numObjs == 0)
		return false;

	TESTopicInfo* topicInfo = firstInfo->infoArray.data[0];
	TESQuest* quest = firstInfo->quest;
	if (!topicInfo || !quest)
		return false;

	//max lip distance so lip sync works at any range
	UInt32* lipDist = GetVoiceLipDistanceLimit();
	UInt32 oldLipDist = *lipDist;
	*lipDist = 0x7FFFFFFF;

	//create DialogueItem manually - this loads response list with voice paths
	void* mem = Engine::GameHeapAlloc(sizeof(DialogueItem));
	if (!mem)
	{
		*lipDist = oldLipDist;
		return false;
	}

	auto* item = ThisCall<DialogueItem*>(0x83C520, mem, quest, topic, topicInfo, speaker); //DialogueItem_0
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

	//fix voice type mismatch - scan sibling folders via BSA lookup
	char fixedPath[512];
	char* finalPath = voicePath;
	if (FixVoicePath(voicePath, fixedPath, sizeof(fixedPath)))
		finalPath = fixedPath;

	BSSoundHandle soundHandle;
	double delay = ThisCall<double>(0x8A20D0, speaker,
		finalPath, &soundHandle,
		emotionType, emotionValue, (UInt32)textLen,
		speakerAnim, listenerAnim, target,
		true, false, false, true, true); //abQueue=false for sync lip load

	//0xAD8E60 skips registration for an invalid handle and can run the callback inline,
	//so a missing voice file must bail before any sayto bookkeeping
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
