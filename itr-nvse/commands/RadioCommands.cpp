//radio track query commands for scripters
//GetPlayingRadioTrack - returns TESSound or TESTopicInfo form
//GetPlayingRadioTrackFileName - returns file path string
//GetPlayingRadioText - returns dialogue text string

#include "RadioCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include <cstring>
#include <cctype>

static NVSEStringVarInterface* g_strInterface = nullptr;

DEFINE_COMMAND_PLUGIN(GetPlayingRadioTrack, "Returns TESSound or TESTopicInfo for the playing radio track", 0, 0, NULL)
DEFINE_COMMAND_PLUGIN(GetPlayingRadioTrackFileName, "Returns file path of the playing radio track", 0, 0, NULL)
DEFINE_COMMAND_PLUGIN(GetPlayingRadioText, "Returns dialogue text of playing radio voice line", 0, 0, NULL)

//internal structs
struct VoiceResponse
{
	String str;
	char* fileName;
};

struct VoiceEntry
{
	tList<void> list00;
	VoiceResponse* response;
	TESTopicInfo* topicInfo;
	TESTopic* topic;
	TESQuest* quest;
	Actor* actor;
};

struct VoiceList
{
	tList<VoiceEntry> entries;
	VoiceList* next;
};

struct RadioData
{
	VoiceList* voiceList;
	UInt32 unk04;
	UInt32 offset;
	UInt32 soundTimeRemaining;
	UInt8 signalStrength;
	UInt8 signalNoiseRatioPct;
	UInt8 gap12[2];
	UInt32 flags;
	tList<void> dynamicRadios;
};

struct RadioEntry
{
	TESObjectREFR* radioRef;
	RadioData data;
};

struct SoundKey
{
	UInt32 soundKey;
	UInt8 byte04;
	UInt8 pad05[3];
	UInt32 unk08;
};

struct DynamicRadio
{
	TESObjectREFR* ref;
	SoundKey sound;
	SoundKey radioStaticSound;
	UInt8 isActive;
	UInt8 pad[3];
};

struct BSGameSound
{
	void* vtbl;
	UInt32 mapKey;
	UInt32 audioFlags;
	UInt32 flags00C;
	UInt32 stateFlags;
	UInt32 duration;
	UInt16 staticAttenuation;
	UInt16 unk01A;
	UInt16 unk01C;
	UInt16 unk01E;
	UInt16 unk020;
	UInt16 unk022;
	float volume;
	float flt028;
	float flt02C;
	UInt32 unk030;
	UInt16 baseSamplingFreq;
	char filePath[254];
};

struct MapEntry
{
	MapEntry* next;
	UInt32 key;
	void* data;
};

struct SoundMap
{
	void* vtbl;
	UInt32 numBuckets;
	MapEntry** buckets;
	UInt32 numItems;

	BSGameSound* Lookup(UInt32 key)
	{
		if (!buckets || !numBuckets) return nullptr;
		for (MapEntry* e = buckets[key % numBuckets]; e; e = e->next)
			if (e->key == key)
				return (BSGameSound*)e->data;
		return nullptr;
	}
};

//game globals
static RadioEntry** g_currentRadio = (RadioEntry**)0x11DD42C;
static char* g_currentSong = (char*)0x11DD448;
static tList<DynamicRadio>* g_dynamicRadios = (tList<DynamicRadio>*)0x11DD58C;

static SoundMap* GetPlayingSoundsMap()
{
	return (SoundMap*)(0x11F6EF0 + 0x54);
}

static const char* GetSoundFilePath(UInt32 soundKey)
{
	if (!soundKey || soundKey == 0xFFFFFFFF) return nullptr;
	BSGameSound* gs = GetPlayingSoundsMap()->Lookup(soundKey);
	return (gs && gs->filePath[0]) ? gs->filePath : nullptr;
}

static VoiceEntry* GetCurrentVoiceEntry()
{
	RadioEntry* radio = *g_currentRadio;
	if (!radio || !radio->data.voiceList) return nullptr;
	auto* node = radio->data.voiceList->entries.Head();
	return (node && node->item) ? node->item : nullptr;
}

//get playing track path - pipboy radio or world radio
static const char* GetPlayingTrackPath()
{
	//pipboy radio song
	if (g_currentSong[0])
		return g_currentSong;

	//world/dynamic radios
	for (auto iter = g_dynamicRadios->Begin(); !iter.End(); ++iter)
	{
		DynamicRadio* dr = iter.Get();
		if (dr && dr->isActive)
		{
			const char* path = GetSoundFilePath(dr->sound.soundKey);
			if (path) return path;
		}
	}

	return nullptr;
}

//normalize runtime path for form matching
//strips "data\sound\" prefix and "_mono"/"_stereo" suffix
static void NormalizePath(const char* src, char* dst, size_t dstSize)
{
	if (!src || !dst || !dstSize) return;
	dst[0] = '\0';

	char temp[512];
	size_t len = strlen(src);
	if (len >= sizeof(temp)) len = sizeof(temp) - 1;
	for (size_t i = 0; i <= len; i++)
		temp[i] = (src[i] == '/') ? '\\' : src[i];

	char* p = temp;

	if (_strnicmp(p, "data\\sound\\", 11) == 0)
		p += 11;

	char* dot = strrchr(p, '.');
	if (dot)
	{
		char* check = dot - 5;
		if (check >= p && _strnicmp(check, "_mono", 5) == 0)
			memmove(check, dot, strlen(dot) + 1);
		else
		{
			check = dot - 7;
			if (check >= p && _strnicmp(check, "_stereo", 7) == 0)
				memmove(check, dot, strlen(dot) + 1);
		}
	}

	len = strlen(p);
	if (len >= dstSize) len = dstSize - 1;
	for (size_t i = 0; i <= len; i++)
		dst[i] = (char)tolower((unsigned char)p[i]);
}

//find TESSound by normalized path
static TESSound* FindSoundByPath(const char* runtimePath)
{
	if (!runtimePath || !runtimePath[0]) return nullptr;

	char normalized[512];
	NormalizePath(runtimePath, normalized, sizeof(normalized));
	if (!normalized[0]) return nullptr;

	UInt8* dataHandler = *(UInt8**)0x11C3F2C;
	if (!dataHandler) return nullptr;

	auto* soundList = (tList<TESSound>*)(dataHandler + 0xD0);
	for (auto iter = soundList->Begin(); !iter.End(); ++iter)
	{
		TESSound* sound = iter.Get();
		if (!sound) continue;

		const char* formPath = sound->soundFile.path.m_data;
		if (!formPath || !formPath[0]) continue;

		char formNorm[512];
		size_t flen = strlen(formPath);
		if (flen >= sizeof(formNorm)) flen = sizeof(formNorm) - 1;
		for (size_t i = 0; i <= flen; i++)
		{
			char c = formPath[i];
			formNorm[i] = (c == '/') ? '\\' : (char)tolower((unsigned char)c);
		}

		if (strcmp(formNorm, normalized) == 0)
			return sound;
	}

	return nullptr;
}

//extract formID from voice filename: RadioNewVe_RNVNewsStory2_0014E82F_1.ogg
static UInt32 ExtractFormIDFromVoicePath(const char* path)
{
	if (!path) return 0;

	const char* fname = path;
	for (const char* p = path; *p; p++)
		if (*p == '\\' || *p == '/') fname = p + 1;

	const char* ext = strrchr(fname, '.');
	if (!ext) return 0;

	const char* lastUs = nullptr;
	for (const char* p = ext - 1; p >= fname; p--)
		if (*p == '_') { lastUs = p; break; }
	if (!lastUs || lastUs <= fname) return 0;

	const char* prevUs = nullptr;
	for (const char* p = lastUs - 1; p >= fname; p--)
		if (*p == '_') { prevUs = p; break; }
	if (!prevUs) return 0;

	int len = (int)(lastUs - prevUs - 1);
	if (len <= 0 || len > 8) return 0;

	char hex[16] = {};
	memcpy(hex, prevUs + 1, len);
	return (UInt32)strtoul(hex, nullptr, 16);
}

//TESTopicInfo::GetResponses at 0x61E780
typedef TESTopicInfoResponse** (__thiscall *_GetResponses)(TESTopicInfo*, UInt32);
static _GetResponses GetResponses = (_GetResponses)0x61E780;

//form lookup
static TESForm* FormLookup(UInt32 refID)
{
	struct Entry { Entry* next; UInt32 key; TESForm* form; };
	UInt8* map = *(UInt8**)0x11C54C0;
	if (!map) return nullptr;
	UInt32 numBuckets = *(UInt32*)(map + 4);
	Entry** buckets = *(Entry***)(map + 8);
	if (!buckets || !numBuckets) return nullptr;
	for (Entry* e = buckets[refID % numBuckets]; e; e = e->next)
		if (e->key == refID) return e->form;
	return nullptr;
}

//command implementations
bool Cmd_GetPlayingRadioTrack_Execute(COMMAND_ARGS)
{
	*result = 0;

	const char* path = GetPlayingTrackPath();
	if (!path) return true;

	TESSound* sound = FindSoundByPath(path);
	if (sound)
	{
		*(UInt32*)result = sound->refID;
		return true;
	}

	VoiceEntry* voice = GetCurrentVoiceEntry();
	if (voice && voice->topicInfo)
	{
		*(UInt32*)result = voice->topicInfo->refID;
		return true;
	}

	UInt32 formID = ExtractFormIDFromVoicePath(path);
	if (formID)
	{
		TESForm* form = FormLookup(formID);
		if (form)
			*(UInt32*)result = form->refID;
	}

	return true;
}

bool Cmd_GetPlayingRadioTrackFileName_Execute(COMMAND_ARGS)
{
	const char* path = GetPlayingTrackPath();
	g_strInterface->Assign(PASS_COMMAND_ARGS, path ? path : "");
	return true;
}

bool Cmd_GetPlayingRadioText_Execute(COMMAND_ARGS)
{
	const char* empty = "";

	const char* path = GetPlayingTrackPath();
	if (!path)
	{
		g_strInterface->Assign(PASS_COMMAND_ARGS, empty);
		return true;
	}

	UInt32 formID = ExtractFormIDFromVoicePath(path);
	if (!formID)
	{
		g_strInterface->Assign(PASS_COMMAND_ARGS, empty);
		return true;
	}

	TESForm* form = FormLookup(formID);
	if (!form || form->typeID != kFormType_INFO)
	{
		g_strInterface->Assign(PASS_COMMAND_ARGS, empty);
		return true;
	}

	TESTopicInfo* info = (TESTopicInfo*)form;
	TESTopicInfoResponse** ppResp = GetResponses(info, 0);
	if (ppResp && *ppResp)
	{
		const char* text = (*ppResp)->responseText.m_data;
		if (text && text[0])
		{
			g_strInterface->Assign(PASS_COMMAND_ARGS, text);
			return true;
		}
	}

	g_strInterface->Assign(PASS_COMMAND_ARGS, empty);
	return true;
}

namespace RadioCommands {
void Init(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	g_strInterface = (NVSEStringVarInterface*)nvseIntf->QueryInterface(kInterface_StringVar);
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterTypedCommand(&kCommandInfo_GetPlayingRadioTrack, kRetnType_Form);
	nvse->RegisterTypedCommand(&kCommandInfo_GetPlayingRadioTrackFileName, kRetnType_String);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterTypedCommand(&kCommandInfo_GetPlayingRadioText, kRetnType_String);
}
}
