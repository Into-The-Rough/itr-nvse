//synthetic dialogue topic commands, valid only while the dialogue menu is open
//AddDialogueTopicEntry - append a row that fires ITR:OnDialogueTopicSelected on click
//SetDialogueTopicEntryAlpha - darken/disable a vanilla topic_<index> row via _line_alpha
//SetDialogueTopicHidden / SetDialogueTopicOrder / ClearDialogueTopicOverrides - vanilla row
//hide + reorder rules, applied on the next list rebuild

#include "DialogueTopicCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameForms.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

#include "internal/globals.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "handlers/OnDialogueMenuBuildHandler.h"

#include <cstdio>
#include <cstring>

namespace {

//0x764430 ListBox::AddEntry: __thiscall, ecx=listbox, builds a row from the template, returns the new Tile*
typedef void* (__thiscall* AddEntry_t)(void* listbox, int index, const char* text, int a4, int a5);
static const AddEntry_t ListBoxAddEntry = (AddEntry_t)0x764430;

//0x700320 Tile::SetFloatTraitValue: __thiscall, ecx=tile, stores value as float
typedef int (__thiscall* SetFloatTrait_t)(void* tile, int traitId, int value);
static const SetFloatTrait_t TileSetFloatTrait = (SetFloatTrait_t)0x700320;

//0xA01860 resolves a named trait to its trait id
typedef int (__cdecl* ResolveTrait_t)(const char* name);
static const ResolveTrait_t ResolveTraitName = (ResolveTrait_t)0xA01860;

constexpr int kTrait_ListIndex = 4012;
constexpr UInt32 kSyntheticFlag = 0x40000000;
constexpr UInt32 kSyntheticIdMax = 0xFFFFFF;

static void* FindChildTile(void* parent, const char* name)
{
	if (!parent) return nullptr;
	for (TileNodeView* node = TileGetFirstChild(parent); node; node = node->next)
	{
		const char* childName = TileGetName(node->data);
		if (childName && !_stricmp(childName, name))
			return node->data;
	}
	return nullptr;
}

}

bool Cmd_AddDialogueTopicEntry_Execute(COMMAND_ARGS)
{
	*result = 0;
	char prompt[512] = { 0 };
	UInt32 syntheticId = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &prompt, &syntheticId))
		return true;

	if (syntheticId > kSyntheticIdMax)
	{
		Log("AddDialogueTopicEntry: syntheticId %u exceeds 24-bit cap", syntheticId);
		return true;
	}

	void* dialogMenu = GetDialogMenu();
	if (!dialogMenu)
	{
		Log("AddDialogueTopicEntry: dialogue menu not open");
		return true;
	}

	//the trait fallback loses low id bits to float precision, so a row without
	//bookkeeping would dispatch a corrupted id - refuse before creating the tile
	if (!OnDialogueMenuBuildHandler::HasSyntheticCapacity())
	{
		Log("AddDialogueTopicEntry: synthetic bookkeeping full, entry refused");
		return true;
	}

	void* listbox = (char*)dialogMenu + 0x40; //ListBox control object
	int index = (int)*(UInt16*)((char*)listbox + 0x1C); //0x1C entry count, engine reads it movzx word (0x7645E4)

	void* tile = ListBoxAddEntry(listbox, index, prompt, 0, 0);
	if (!tile)
	{
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

static ParamInfo kParams_AddDialogueTopicEntry[2] = {
	{ "prompt", kParamType_String, 0 },
	{ "syntheticId", kParamType_Integer, 0 }
};

static ParamInfo kParams_SetDialogueTopicEntryAlpha[2] = {
	{ "index", kParamType_Integer, 0 },
	{ "alpha", kParamType_Integer, 0 }
};

static CommandInfo kCommandInfo_AddDialogueTopicEntry = {
	"AddDialogueTopicEntry", "", 0,
	"Appends a synthetic row to the open dialogue menu that fires ITR:OnDialogueTopicSelected on click",
	0, 2, kParams_AddDialogueTopicEntry, Cmd_AddDialogueTopicEntry_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_SetDialogueTopicEntryAlpha = {
	"SetDialogueTopicEntryAlpha", "", 0,
	"Sets the _line_alpha of vanilla dialogue row topic_<index> to darken or disable it",
	0, 2, kParams_SetDialogueTopicEntryAlpha, Cmd_SetDialogueTopicEntryAlpha_Execute, nullptr, nullptr, 0
};

static ParamInfo kParams_SetDialogueTopicHidden[2] = {
	{ "topicOrInfo", kParamType_AnyForm, 0 },
	{ "hidden", kParamType_Integer, 0 }
};

static ParamInfo kParams_SetDialogueTopicOrder[2] = {
	{ "topicOrInfo", kParamType_AnyForm, 0 },
	{ "order", kParamType_Integer, 0 }
};

static CommandInfo kCommandInfo_SetDialogueTopicHidden = {
	"SetDialogueTopicHidden", "", 0,
	"Hides or shows a dialogue row by its TESTopic or TESTopicInfo, applied on the next list rebuild (menu open or after each reply)",
	0, 2, kParams_SetDialogueTopicHidden, Cmd_SetDialogueTopicHidden_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_SetDialogueTopicOrder = {
	"SetDialogueTopicOrder", "", 0,
	"Sorts a dialogue row by its TESTopic or TESTopicInfo, lower orders first, applied on the next list rebuild (menu open or after each reply)",
	0, 2, kParams_SetDialogueTopicOrder, Cmd_SetDialogueTopicOrder_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_ClearDialogueTopicOverrides = {
	"ClearDialogueTopicOverrides", "", 0,
	"Clears all dialogue topic hide and order rules, returns the number removed, applied on the next list rebuild",
	0, 0, nullptr, Cmd_ClearDialogueTopicOverrides_Execute, nullptr, nullptr, 0
};

namespace DialogueTopicCommands {
void RegisterCommands(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->RegisterCommand(&kCommandInfo_AddDialogueTopicEntry);
	nvseIntf->RegisterCommand(&kCommandInfo_SetDialogueTopicEntryAlpha);
	nvseIntf->RegisterCommand(&kCommandInfo_SetDialogueTopicHidden);
	nvseIntf->RegisterCommand(&kCommandInfo_SetDialogueTopicOrder);
	nvseIntf->RegisterCommand(&kCommandInfo_ClearDialogueTopicOverrides);
}
}
