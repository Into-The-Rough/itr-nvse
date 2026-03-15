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
#include <cstring>
#include <cstdio>
#include <windows.h>

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

#define GameHeapAlloc(size) ((void*(__thiscall*)(void*, UInt32))(0xAA3E40))((void*)0x11F6238, size)
#define GameHeapFree(ptr) ((void(__thiscall*)(void*, void*))(0xAA4060))((void*)0x11F6238, ptr)

struct BSSoundHandle
{
	UInt32 uiSoundID;
	bool bAssumeSuccess;
	UInt32 uiState;
	BSSoundHandle() : uiSoundID(0xFFFFFFFF), bAssumeSuccess(false), uiState(0) {}
};

struct BSHash { UInt8 pad[8]; };

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

bool Cmd_ForceSay_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESTopic* topic = nullptr;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &topic, &target))
		return true;
	if (!topic || !thisObj)
		return true;

	if (thisObj->typeID != kFormType_ACHR && thisObj->typeID != kFormType_ACRE)
		return true;
	Actor* speaker = (Actor*)thisObj;

	void* process = ThisCall<void*>(0x8D8520, speaker); //MobileObject::GetCurrentProcess
	if (!process)
		return true;
	if (!speaker->GetNiNode())
		return true;

	//walk topic->infos to get first topicinfo + quest directly (bypass all conditions)
	TESTopic::Info* firstInfo = *(TESTopic::Info**)((UInt8*)topic + 0x2C);
	if (!firstInfo)
		return true;

	if (firstInfo->infoArray.numObjs == 0)
		return true;

	TESTopicInfo* topicInfo = firstInfo->infoArray.data[0];
	TESQuest* quest = firstInfo->quest;
	if (!topicInfo || !quest)
		return true;

	//max lip distance so lip sync works at any range
	UInt32* lipDist = (UInt32*)(0x11CD7D4 + 4);
	UInt32 oldLipDist = *lipDist;
	*lipDist = 0x7FFFFFFF;

	//create DialogueItem manually - this loads response list with voice paths
	void* mem = GameHeapAlloc(0x1C);
	if (!mem)
	{
		*lipDist = oldLipDist;
		return true;
	}

	void* item = ThisCall<void*>(0x83C520, mem, quest, topic, topicInfo, speaker); //DialogueItem_0
	ThisCall<bool>(0x83C7B0, item); //FirstResponse
	void* response = ThisCall<void*>(0x83C820, item); //GetCurrentItem

	if (!response)
	{
		Log("ForceSay: no response found for topic %08X", topic->refID);
		ThisCall(0x83C670, item);
		GameHeapFree(item);
		*lipDist = oldLipDist;
		return true;
	}

	//layout: [BSStringT text +0x00][emotion +0x08/+0x0C][BSStringT voice +0x10][anims +0x18/+0x1C]
	char* voicePath       = *(char**)((UInt8*)response + 0x10);
	UInt32 emotionType    = *(UInt32*)((UInt8*)response + 0x08);
	UInt32 emotionValue   = *(UInt32*)((UInt8*)response + 0x0C);
	UInt16 textLen        = *(UInt16*)((UInt8*)response + 0x04);
	void* speakerAnim     = *(void**)((UInt8*)response + 0x18);
	void* listenerAnim    = *(void**)((UInt8*)response + 0x1C);

	if (!voicePath || !voicePath[0])
	{
		Log("ForceSay: empty voice path, aborting");
		ThisCall(0x83C670, item);
		GameHeapFree(item);
		*lipDist = oldLipDist;
		return true;
	}

	//fix voice type mismatch - scan sibling folders via BSA lookup
	char fixedPath[512];
	char* finalPath = voicePath;
	if (FixVoicePath(voicePath, fixedPath, sizeof(fixedPath)))
	{
		Log("ForceSay: redirected %s -> %s", voicePath, fixedPath);
		finalPath = fixedPath;
	}

	Log("ForceSay: playing %s", finalPath);

	BSSoundHandle soundHandle;
	double delay = ThisCall<double>(0x8A20D0, speaker,
		finalPath, &soundHandle,
		emotionType, emotionValue, (UInt32)textLen,
		speakerAnim, listenerAnim, target,
		true, false, false, true, true); //abQueue=false for sync lip load

	Log("ForceSay: delay=%.2f soundID=%u", delay, soundHandle.uiSoundID);

	ThisCall(0x57AD20, speaker, topic);     //SetSayToTopic
	ThisCall(0x57ACE0, speaker, topicInfo); //SetSayToTopicInfo
	ThisCall(0x57AD60, speaker, 1);         //SetSayToResponseNumber

	ThisCall(0xAD8E60, &soundHandle,
		(void*)0x936A20,                    //Actor::SayToCallBack
		(void*)(speaker->refID));

	ThisCall(0x8D8DC0, process, (UInt8)1);  //SetDoingSayTo

	ThisCall(0x83C850, item, 0);            //RunResult(TIRS_BEGIN)

	*lipDist = oldLipDist;

	ThisCall(0x83C670, item);
	GameHeapFree(item);

	Log("ForceSay: %08X saying topic %08X (info %08X)", speaker->refID, topic->refID, topicInfo->refID);
	*result = 1;
	return true;
}

void ForceSayCommand_RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceSay);
}
