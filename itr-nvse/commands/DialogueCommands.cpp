#include "DialogueCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"

#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/MenuLayout.h"
#include "internal/globals.h"
#include "handlers/DialogueTextFilter.h"
#include "handlers/OnDialogueMenuBuildHandler.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <Windows.h>

extern const _ExtractArgs ExtractArgs;
extern NVSEArrayVarInterface* g_arrInterface;

namespace {

	NVSEStringVarInterface* g_strInterface = nullptr;

	constexpr UInt8 kFormType_TopicInfo = 0x46;

	struct BSHash { UInt8 pad[8]; };

	//0x764430 ListBox::AddEntry: __thiscall, ecx=listbox, builds a row from the template, returns the new Tile*
	using AddEntry_t = void*(__thiscall*)(void* listbox, int index, const char* text, int a4, int a5);
	const AddEntry_t ListBoxAddEntry = (AddEntry_t)0x764430;

	//0x700320 Tile::SetFloatTraitValue: __thiscall, ecx=tile, stores value as float
	using SetFloatTrait_t = int(__thiscall*)(void* tile, int traitId, int value);
	const SetFloatTrait_t TileSetFloatTrait = (SetFloatTrait_t)0x700320;

	//0xA01860 resolves a named trait to its trait id
	using ResolveTrait_t = int(__cdecl*)(const char* name);
	const ResolveTrait_t ResolveTraitName = (ResolveTrait_t)0xA01860;

	constexpr int kTrait_ListIndex = 4012;
	constexpr UInt32 kSyntheticFlag = 0x40000000;
	constexpr UInt32 kSyntheticIdMax = 0xFFFFFF;

	void* FindChildTile(void* parent, const char* name)
	{
		if (!parent) return nullptr;
		for (TileNodeView* node = TileGetFirstChild(parent); node; node = node->next) {
			const char* childName = TileGetName(node->data);
			if (childName && !_stricmp(childName, name))
				return node->data;
		}
		return nullptr;
	}

	TESTopic::Info* GetFirstTopicInfo(TESTopic* topic)
	{
		auto* head = reinterpret_cast<TESTopicInfoListNodeView*>(&topic->infos);
		return head->item;
	}

	bool ExistsInBSA(const char* path)
	{
		const char* bsaPath = path;
		if (_strnicmp(bsaPath, "Data\\", 5) == 0)
			bsaPath += 5;

		BSHash dirHash, fileHash;
		CdeclCall(0xAFD270, bsaPath, &dirHash, &fileHash); //BSHash::MakeDirAndFile
		void* entry = CdeclCall<void*>(0xAF6540, 4, &dirHash, &fileHash, bsaPath); //Voices
		if (entry) return true;
		entry = CdeclCall<void*>(0xAF6540, 3, &dirHash, &fileHash, bsaPath); //Sounds
		return entry != nullptr;
	}

	bool VoiceFileExists(const char* path)
	{
		DWORD attr = GetFileAttributesA(path);
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
			return true;
		return ExistsInBSA(path);
	}

	//voice path: Data\Sound\Voice\<plugin>\<voice type edid>\<filename> (0x616FA0)
	//when the speaker's voice type never recorded the line, borrow any voice type that did
	bool ResolveVoicePath(const char* voicePath, char* out, UInt32 outSize)
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
		for (const char* p = voicePath; *p; p++) {
			if (*p == '\\') {
				slashCount++;
				if (slashCount == 4) seg4start = p + 1;
				if (slashCount == 5) {
					seg5 = p;
					break;
				}
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
		for (auto iter = dataHandler->voiceTypeList.Begin(); !iter.End(); ++iter) {
			const char* edid = Engine::TESForm_GetEditorID(iter.Get());
			if (!edid || !*edid)
				continue;
			snprintf(out, outSize, "%.*s%s%s", prefixLen, voicePath, edid, filename);
			if (VoiceFileExists(out)) {
				Log("ForceSay: borrowed voice %s for %s", edid, voicePath);
				return true;
			}
		}
		return false;
	}

	bool ForceSay(Actor* speaker, TESTopic* topic, Actor* target)
	{
		if (!speaker || !topic)
			return false;

		if (speaker->typeID != kFormType_ACHR && speaker->typeID != kFormType_ACRE)
			return false;

		void* process = Engine::Actor_GetProcess(speaker);
		if (!process || !speaker->GetNiNode())
			return false;

		//the line pick Say uses via 0x8A1BD0 (quest running, conditions, say once, random), it has no death check
		auto* item = ThisCall<DialogueItem*>(0x61B320, topic, speaker, target, 0, 0, 0);
		if (!item) {
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

		if (!response) {
			ThisCall(0x83C670, item);
			Engine::GameHeapFree(item);
			*lipDist = oldLipDist;
			return false;
		}

		char* voicePath = response->voiceFileName.m_data;
		UInt32 emotionType = response->emotionType;
		UInt32 emotionValue = static_cast<UInt32>(response->emotionValue);
		UInt16 textLen = response->responseText.m_dataLen;
		TESIdleForm* speakerAnim = response->speakerIdle;
		TESIdleForm* listenerAnim = response->listenerIdle;

		if (!voicePath || !voicePath[0]) {
			ThisCall(0x83C670, item);
			Engine::GameHeapFree(item);
			*lipDist = oldLipDist;
			return false;
		}

		//unvoiced lines keep the original path, play as silent subtitle and run result script
		char finalPath[512];
		if (!ResolveVoicePath(voicePath, finalPath, sizeof(finalPath)))
			snprintf(finalPath, sizeof(finalPath), "%s", voicePath);

		BSSoundHandleView soundHandle = {0xFFFFFFFF, false, 0};
		ThisCall<double>(0x8A20D0, speaker,
			finalPath, &soundHandle,
			emotionType, emotionValue, (UInt32)textLen,
			speakerAnim, listenerAnim, target,
			true, false, false, true, true); //abQueue=false for sync lip load

		//0xAD8E60 skips registration for an invalid handle and can run the callback inline,
		//so a refused line (no 3d, severed head or brain) must bail before any sayto bookkeeping
		if (soundHandle.uiSoundID == 0xFFFFFFFF) {
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

DEFINE_COMMAND_PLUGIN(GetDialogueInfoFlags, "Returns combined flags for a dialogue info", 0, 1, kParams_OneForm)
DEFINE_COMMAND_PLUGIN(SetDialogueInfoFlags, "Sets combined flags for a dialogue info (runtime only)", 0, 2, kParams_OneForm_OneInt)
DEFINE_COMMAND_PLUGIN(GetDisplayedDialogueInfos, "Returns array of dialogue infos currently shown in dialogue menu", 0, 0, nullptr)

static ParamInfo kParams_ForceSay[] = {
	{"topic", kParamType_Topic, 0},
	{"target", kParamType_Actor, 1},
};
DEFINE_COMMAND_PLUGIN(ForceSay, "Force an actor to say a topic line, dead or alive", 1, 2, kParams_ForceSay)

static ParamInfo kParams_IsSaying[] = {
	{"topicOrInfo", kParamType_AnyForm, 0},
};
DEFINE_COMMAND_PLUGIN(IsSaying, "Check if actor is currently saying a specific topic or topic info", 1, 1, kParams_IsSaying)

DEFINE_COMMAND_PLUGIN(GetCurrentDialogue, "Returns the dialogue line the actor is speaking right now, empty string when silent", 1, 0, NULL)

bool Cmd_GetDialogueInfoFlags_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &form))
		return true;

	if (!form || form->typeID != kFormType_TopicInfo)
		return true;

	auto* info = reinterpret_cast<TESTopicInfo*>(form);
	*result = info->flags1 | (info->flags2 << 8);
	return true;
}

bool Cmd_SetDialogueInfoFlags_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	UInt32 flags = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &form, &flags))
		return true;

	if (!form || form->typeID != kFormType_TopicInfo)
		return true;

	auto* info = reinterpret_cast<TESTopicInfo*>(form);
	info->flags1 = flags & 0xFF;
	info->flags2 = (flags >> 8) & 0xFF;

	*result = 1;
	return true;
}

bool Cmd_GetDisplayedDialogueInfos_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!g_arrInterface || !IsDialogMenuOpen())
		return true;

	void* dialogMenu = GetDialogMenu();
	if (!dialogMenu)
		return true;

	void* menuTopicMgr = DialogMenuGetTopicManager(dialogMenu);
	if (!menuTopicMgr)
		return true;

	NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	if (!arr)
		return true;

	//hidden rows stay linked past the rendered run. walking the chain directly leaves the manager
	//cursor the engine resolves clicks through untouched
	const UInt32 rowCount = OnDialogueMenuBuildHandler::GetRenderedRowCount();
	UInt32 walked = 0;

	MenuTopicNodeView* head = MenuTopicManagerGetTopicList(menuTopicMgr);
	for (MenuTopicNodeView* node = head ? head->next : nullptr; node && node->menuTopic; node = node->next) {
		if (rowCount != OnDialogueMenuBuildHandler::kRowCountUnknown && walked >= rowCount)
			break;
		walked++;

		TESTopicInfo* topicInfo = MenuTopicGetTopicInfo(node->menuTopic);
		if (topicInfo) {
			NVSEArrayVarInterface::Element elem(topicInfo);
			g_arrInterface->AppendElement(arr, elem);
		}
	}

	g_arrInterface->AssignCommandResult(arr, result);
	return true;
}

bool Cmd_ForceSay_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESTopic* topic = nullptr;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &topic, &target))
		return true;

	if (thisObj && ForceSay((Actor*)thisObj, topic, target))
		*result = 1;

	return true;
}

bool Cmd_IsSaying_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* form = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &form))
		return true;
	if (!form || !thisObj)
		return true;
	if (thisObj->typeID != kFormType_ACHR && thisObj->typeID != kFormType_ACRE)
		return true;

	if (form->typeID == kFormType_DIAL) {
		TESTopic* cur = ThisCall<TESTopic*>(0x57BD40, thisObj); //Actor::GetExtraSayToTopic
		if (cur == (TESTopic*)form)
			*result = 1;
	} else if (form->typeID == kFormType_INFO) {
		TESTopicInfo* cur = ThisCall<TESTopicInfo*>(0x934430, thisObj); //Actor::GetExtraSayToTopicInfo
		if (cur == (TESTopicInfo*)form)
			*result = 1;
	}

	return true;
}

bool Cmd_AddDialogueTopicEntry_Execute(COMMAND_ARGS)
{
	*result = 0;
	char prompt[512] = {0};
	UInt32 syntheticId = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &prompt, &syntheticId))
		return true;

	if (syntheticId > kSyntheticIdMax) {
		Log("AddDialogueTopicEntry: syntheticId %u exceeds 24-bit cap", syntheticId);
		return true;
	}

	void* dialogMenu = GetDialogMenu();
	if (!dialogMenu) {
		Log("AddDialogueTopicEntry: dialogue menu not open");
		return true;
	}

	//the trait fallback loses low id bits to float precision, so a row without
	//bookkeeping would dispatch a corrupted id - refuse before creating the tile
	if (!OnDialogueMenuBuildHandler::HasSyntheticCapacity()) {
		Log("AddDialogueTopicEntry: synthetic bookkeeping full, entry refused");
		return true;
	}

	void* listbox = (char*)dialogMenu + 0x40; //ListBox control object
	int index = (int)*(UInt16*)((char*)listbox + 0x1C); //0x1C entry count, engine reads it movzx word (0x7645E4)

	void* tile = ListBoxAddEntry(listbox, index, prompt, 0, 0);
	if (!tile) {
		Log("AddDialogueTopicEntry: AddEntry returned null");
		return true;
	}

	TileSetFloatTrait(tile, ResolveTraitName("_line_alpha"), 255);
	TileSetFloatTrait(tile, kTrait_ListIndex, (int)(syntheticId | kSyntheticFlag));
	OnDialogueMenuBuildHandler::RegisterSyntheticTile(tile, syntheticId);

	*result = 1;
	return true;
}

bool Cmd_SetDialogueTopicEntryAlpha_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 index = 0;
	UInt32 alpha = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &index, &alpha))
		return true;

	void* dialogMenu = GetDialogMenu();
	if (!dialogMenu)
		return true;

	void* listbox = (char*)dialogMenu + 0x40;
	void* listTile = *(void**)((char*)listbox + 0x0C); //0x0C list container tile

	char name[64];
	sprintf_s(name, sizeof(name), "topic_%u", index);

	void* tile = FindChildTile(listTile, name);
	if (!tile)
		return true;

	TileSetFloatTrait(tile, ResolveTraitName("_line_alpha"), (int)alpha);
	*result = 1;
	return true;
}

bool Cmd_SetDialogueTopicHidden_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	UInt32 hidden = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &form, &hidden))
		return true;
	if (!form)
		return true;

	if (OnDialogueMenuBuildHandler::SetTopicHidden(form->refID, hidden != 0))
		*result = 1;
	return true;
}

bool Cmd_SetDialogueTopicOrder_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	UInt32 order = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &form, &order))
		return true;
	if (!form)
		return true;

	if (OnDialogueMenuBuildHandler::SetTopicOrder(form->refID, (int)order))
		*result = 1;
	return true;
}

bool Cmd_ClearDialogueTopicOverrides_Execute(COMMAND_ARGS)
{
	*result = (double)OnDialogueMenuBuildHandler::ClearTopicOverrides();
	return true;
}

bool Cmd_GetCurrentDialogue_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!g_strInterface)
		return true;

	const char* text = nullptr;
	if (thisObj && (thisObj->typeID == kFormType_ACHR || thisObj->typeID == kFormType_ACRE))
		text = DialogueTextFilter::GetSpokenLine(thisObj->refID);

	g_strInterface->Assign(PASS_COMMAND_ARGS, text ? text : "");
	return true;
}

static ParamInfo kParams_AddDialogueTopicEntry[2] = {
	{"prompt", kParamType_String, 0},
	{"syntheticId", kParamType_Integer, 0}};

static ParamInfo kParams_SetDialogueTopicEntryAlpha[2] = {
	{"index", kParamType_Integer, 0},
	{"alpha", kParamType_Integer, 0}};

static ParamInfo kParams_SetDialogueTopicHidden[2] = {
	{"topicOrInfo", kParamType_AnyForm, 0},
	{"hidden", kParamType_Integer, 0}};

static ParamInfo kParams_SetDialogueTopicOrder[2] = {
	{"topicOrInfo", kParamType_AnyForm, 0},
	{"order", kParamType_Integer, 0}};

static CommandInfo kCommandInfo_AddDialogueTopicEntry = {
	"AddDialogueTopicEntry", "", 0,
	"Appends a synthetic row to the open dialogue menu that fires ITR:OnDialogueTopicSelected on click",
	0, 2, kParams_AddDialogueTopicEntry, Cmd_AddDialogueTopicEntry_Execute, nullptr, nullptr, 0};

static CommandInfo kCommandInfo_SetDialogueTopicEntryAlpha = {
	"SetDialogueTopicEntryAlpha", "", 0,
	"Sets the _line_alpha of vanilla dialogue row topic_<index> to darken or disable it",
	0, 2, kParams_SetDialogueTopicEntryAlpha, Cmd_SetDialogueTopicEntryAlpha_Execute, nullptr, nullptr, 0};

static CommandInfo kCommandInfo_SetDialogueTopicHidden = {
	"SetDialogueTopicHidden", "", 0,
	"Hides or shows a dialogue row by its TESTopic or TESTopicInfo, applied on the next list rebuild (menu open or after each reply)",
	0, 2, kParams_SetDialogueTopicHidden, Cmd_SetDialogueTopicHidden_Execute, nullptr, nullptr, 0};

static CommandInfo kCommandInfo_SetDialogueTopicOrder = {
	"SetDialogueTopicOrder", "", 0,
	"Sorts a dialogue row by its TESTopic or TESTopicInfo, lower orders first, applied on the next list rebuild (menu open or after each reply)",
	0, 2, kParams_SetDialogueTopicOrder, Cmd_SetDialogueTopicOrder_Execute, nullptr, nullptr, 0};

static CommandInfo kCommandInfo_ClearDialogueTopicOverrides = {
	"ClearDialogueTopicOverrides", "", 0,
	"Clears all dialogue topic hide and order rules, returns the number removed, applied on the next list rebuild",
	0, 0, nullptr, Cmd_ClearDialogueTopicOverrides_Execute, nullptr, nullptr, 0};

namespace DialogueCommands {

	void Init(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		g_strInterface = (NVSEStringVarInterface*)nvse->QueryInterface(kInterface_StringVar);
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_GetDialogueInfoFlags);
		nvse->RegisterCommand(&kCommandInfo_SetDialogueInfoFlags);
		nvse->RegisterTypedCommand(&kCommandInfo_GetDisplayedDialogueInfos, kRetnType_Array);
	}

	void RegisterForceSayCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_ForceSay);
	}

	void RegisterIsSayingCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_IsSaying);
	}

	void RegisterTopicCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_AddDialogueTopicEntry);
		nvse->RegisterCommand(&kCommandInfo_SetDialogueTopicEntryAlpha);
		nvse->RegisterCommand(&kCommandInfo_SetDialogueTopicHidden);
		nvse->RegisterCommand(&kCommandInfo_SetDialogueTopicOrder);
		nvse->RegisterCommand(&kCommandInfo_ClearDialogueTopicOverrides);
	}

	void RegisterCurrentDialogueCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterTypedCommand(&kCommandInfo_GetCurrentDialogue, kRetnType_String);
	}

}
