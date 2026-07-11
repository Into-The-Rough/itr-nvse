//dialogue menu build event plus synthetic topic click routing
//build fires at DialogMenu::LoadTopicsList exit, on menu open and after each reply
//synthetic rows carry trait 4012 bit 0x40000000, which is out of range for the
//select-by-index guard sub_83E430, so a missed synthetic click is a vanilla no-op

#include "OnDialogueMenuBuildHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

class TESObjectREFR;

namespace OnDialogueMenuBuildHandler {

constexpr char kEventBuild[]    = "ITR:OnDialogueMenuBuild";
constexpr char kEventSelected[] = "ITR:OnDialogueTopicSelected";

constexpr UInt32 kSyntheticFlag = 0x40000000; //trait 4012 discriminator, out of sub_83E430 index range
constexpr int kTrait_ListIndex = 4012;

constexpr UInt32 kAddr_LoadTopicsList = 0x7638B0; //DialogMenu::LoadTopicsList, __thiscall(this)
constexpr UInt32 kAddr_TopicClick     = 0x7624F0; //DialogMenu topic click handler

//0xA011B0 Tile::GetFloatTraitValue: __thiscall, ecx=tile, stack=traitID, returns float value as double
typedef double(__thiscall* GetFloatTrait_t)(void* tile, int traitID);
static const GetFloatTrait_t TileGetFloatTrait = (GetFloatTrait_t)0xA011B0;

typedef int(__thiscall* LoadTopicsList_t)(void* dialogMenu);
typedef unsigned int(__fastcall* TopicClick_t)(void* dialogMenu, void* edx, int a3, void* tile);

static Detours::JumpDetour s_buildDetour;
static Detours::JumpDetour s_clickDetour;
static LoadTopicsList_t s_origLoadTopics = nullptr;
static TopicClick_t s_origTopicClick = nullptr;

struct SyntheticEntry { void* tile; UInt32 id; };
static const int kMaxSynthetic = 16;
static SyntheticEntry s_synthetic[kMaxSynthetic];
static int s_syntheticCount = 0;
static CRITICAL_SECTION s_lock;
static volatile LONG s_lockInit = 0;

//our build/click dispatches run synchronously inside engine menu routines. a handler that
//triggers another build or synthetic click would nest our dispatch - guard against it. main
//thread only, so a plain bool is sufficient
static bool s_inDispatch = false;

static void EnsureLock() { InitCriticalSectionOnce(&s_lockInit, &s_lock); }

//topic hide/order rules, session-persistent, flushed only by ClearRules on game load
constexpr int kMaxHideRules = 32;
constexpr int kMaxOrderRules = 32;
constexpr int kMaxVisibleNodes = 64;
constexpr int kMaxHiddenNodes = 32;
constexpr int kNoOrder = 0x7FFFFFFF; //default sort key, keeps original relative order

struct OrderRule { UInt32 formID; int order; };
static UInt32 s_hideRules[kMaxHideRules];
static int s_hideCount = 0;
static OrderRule s_orderRules[kMaxOrderRules];
static int s_orderCount = 0;
static CRITICAL_SECTION s_ruleLock;
static volatile LONG s_ruleLockInit = 0;

static void EnsureRuleLock() { InitCriticalSectionOnce(&s_ruleLockInit, &s_ruleLock); }

//8-byte tList node, null-terminated. data is MenuTopic*
struct TopicNode { void* data; TopicNode* next; };

//nodes unlinked before the trampoline, relinked onto the tail after it
static TopicNode* s_hiddenHold[kMaxHiddenNodes];
static int s_hiddenCount = 0;

//assumes s_ruleLock held
static bool MenuTopicMatches(void* menuTopic, UInt32 formID)
{
	if (!menuTopic || !formID) return false;
	void* info = *(void**)((char*)menuTopic + 0x18); //TESTopicInfo*
	void* topic = *(void**)((char*)menuTopic + 0x1C); //TESTopic*
	UInt32 infoID = info ? *(UInt32*)((char*)info + 0x0C) : 0; //TESForm::refID
	UInt32 topicID = topic ? *(UInt32*)((char*)topic + 0x0C) : 0;
	return infoID == formID || topicID == formID;
}

static bool IsMenuTopicHidden(void* menuTopic)
{
	for (int i = 0; i < s_hideCount; i++)
		if (MenuTopicMatches(menuTopic, s_hideRules[i])) return true;
	return false;
}

static int GetMenuTopicOrder(void* menuTopic)
{
	for (int i = 0; i < s_orderCount; i++)
		if (MenuTopicMatches(menuTopic, s_orderRules[i].formID)) return s_orderRules[i].order;
	return kNoOrder;
}

bool SetTopicHidden(UInt32 formID, bool hidden)
{
	if (!formID) return false;
	EnsureRuleLock();
	ScopedLock lock(&s_ruleLock);
	for (int i = 0; i < s_hideCount; i++)
	{
		if (s_hideRules[i] == formID)
		{
			if (!hidden) { s_hideRules[i] = s_hideRules[--s_hideCount]; }
			return true;
		}
	}
	if (!hidden) return true; //already absent
	if (s_hideCount >= kMaxHideRules) { Log("SetDialogueTopicHidden: hide rule store full"); return false; }
	s_hideRules[s_hideCount++] = formID;
	return true;
}

bool SetTopicOrder(UInt32 formID, int order)
{
	if (!formID) return false;
	EnsureRuleLock();
	ScopedLock lock(&s_ruleLock);
	for (int i = 0; i < s_orderCount; i++)
	{
		if (s_orderRules[i].formID == formID) { s_orderRules[i].order = order; return true; }
	}
	if (s_orderCount >= kMaxOrderRules) { Log("SetDialogueTopicOrder: order rule store full"); return false; }
	s_orderRules[s_orderCount].formID = formID;
	s_orderRules[s_orderCount].order = order;
	s_orderCount++;
	return true;
}

int ClearTopicOverrides()
{
	EnsureRuleLock();
	ScopedLock lock(&s_ruleLock);
	int removed = s_hideCount + s_orderCount;
	s_hideCount = 0;
	s_orderCount = 0;
	return removed;
}

void ClearRules()
{
	EnsureRuleLock();
	ScopedLock lock(&s_ruleLock);
	s_hideCount = 0;
	s_orderCount = 0;
}

//unlink hidden nodes and stable-sort the visible ones by order rule, sentinel untouched
//returns true when the chain was modified, held-aside nodes stored in s_hiddenHold
static bool ApplyTopicRules(void* dialogMenu)
{
	EnsureRuleLock();
	ScopedLock lock(&s_ruleLock);

	s_hiddenCount = 0;
	if (s_hideCount == 0 && s_orderCount == 0) return false;

	void* manager = (char*)dialogMenu + 0x70; //MenuTopicManager
	TopicNode* sentinel = *(TopicNode**)((char*)manager + 0x4); //head node, greeting bucket
	if (!sentinel) return false;
	TopicNode* first = sentinel->next; //never move the sentinel
	if (!first) return false;

	TopicNode* visible[kMaxVisibleNodes];
	int visCount = 0;
	for (TopicNode* cur = first; cur; cur = cur->next)
	{
		bool hide = IsMenuTopicHidden(cur->data);
		if (hide)
		{
			if (s_hiddenCount < kMaxHiddenNodes) { s_hiddenHold[s_hiddenCount++] = cur; continue; }
			Log("ApplyTopicRules: hidden node cap reached, keeping row visible");
			hide = false;
		}
		if (visCount >= kMaxVisibleNodes)
		{
			//exceeds sort capacity - leave the chain untouched, unlink nothing
			s_hiddenCount = 0;
			Log("ApplyTopicRules: topic list exceeds %d visible rows, skipping reorder", kMaxVisibleNodes);
			return false;
		}
		visible[visCount++] = cur;
	}

	//stable insertion sort ascending by order, ties keep original sequence
	for (int i = 1; i < visCount; i++)
	{
		TopicNode* key = visible[i];
		int keyOrder = GetMenuTopicOrder(key->data);
		int j = i - 1;
		while (j >= 0 && GetMenuTopicOrder(visible[j]->data) > keyOrder)
		{
			visible[j + 1] = visible[j];
			j--;
		}
		visible[j + 1] = key;
	}

	TopicNode* prev = sentinel;
	for (int i = 0; i < visCount; i++) { prev->next = visible[i]; prev = visible[i]; }
	prev->next = nullptr;
	return true;
}

//relink held-aside hidden nodes onto the chain tail so the engine clear frees them
//no-op when nothing was held - must leave s_hiddenHold empty on every path
static void RestoreHiddenNodes(void* dialogMenu)
{
	if (s_hiddenCount == 0) return;

	void* manager = (char*)dialogMenu + 0x70; //MenuTopicManager
	TopicNode* head = *(TopicNode**)((char*)manager + 0x4);
	if (head)
	{
		TopicNode* tail = head;
		while (tail->next) tail = tail->next;
		for (int i = 0; i < s_hiddenCount; i++) { tail->next = s_hiddenHold[i]; tail = s_hiddenHold[i]; }
		tail->next = nullptr;
	}
	s_hiddenCount = 0;
}

bool HasSyntheticCapacity()
{
	EnsureLock();
	ScopedLock lock(&s_lock);
	return s_syntheticCount < kMaxSynthetic;
}

bool RegisterSyntheticTile(void* tile, UInt32 syntheticId)
{
	if (!tile) return false;
	EnsureLock();
	ScopedLock lock(&s_lock);
	if (s_syntheticCount >= kMaxSynthetic) return false;
	s_synthetic[s_syntheticCount].tile = tile;
	s_synthetic[s_syntheticCount].id = syntheticId;
	s_syntheticCount++;
	return true;
}

static bool LookupSynthetic(void* tile, UInt32& idOut)
{
	ScopedLock lock(&s_lock);
	for (int i = 0; i < s_syntheticCount; i++)
		if (s_synthetic[i].tile == tile) { idOut = s_synthetic[i].id; return true; }
	return false;
}

void ClearState()
{
	EnsureLock();
	ScopedLock lock(&s_lock);
	s_syntheticCount = 0;
}

static void* GetSpeaker(void* dialogMenu)
{
	return *(void**)((char*)dialogMenu + 0x80); //0x80 speaker Actor*
}

//vanilla row count from the listbox running counter, set per entry in ListBox::AddEntry
static UInt32 GetVanillaTopicCount(void* dialogMenu)
{
	void* listbox = (char*)dialogMenu + 0x40; //ListBox control object
	return *(UInt32*)((char*)listbox + 0x1C); //0x1C entry count
}

static int __fastcall Hook_LoadTopicsList(void* dialogMenu, void* /*edx*/)
{
	if (dialogMenu) ApplyTopicRules(dialogMenu);

	int result = s_origLoadTopics ? s_origLoadTopics(dialogMenu) : 0;

	if (dialogMenu) RestoreHiddenNodes(dialogMenu);

	ClearState();

	if (g_eventManagerInterface && !g_isLoadingSave && dialogMenu && !s_inDispatch)
	{
		void* speaker = GetSpeaker(dialogMenu);
		UInt32 count = GetVanillaTopicCount(dialogMenu);
		s_inDispatch = true;
		g_eventManagerInterface->DispatchEvent(kEventBuild, (TESObjectREFR*)speaker, speaker, (int)count);
		s_inDispatch = false;
	}

	return result;
}

static unsigned int __fastcall Hook_TopicClick(void* dialogMenu, void* edx, int a3, void* tile)
{
	//a3 == -1 is the click path, tile is the clicked row
	//mirror the vanilla selectable-state guard so synthetic rows cannot fire while
	//the menu is idle (0) or processing a reply (4)
	UInt32 menuState = dialogMenu ? *(UInt32*)((char*)dialogMenu + 0x28) : 0; //0x28 menu state
	if (a3 == -1 && tile && menuState && menuState != 4)
	{
		UInt32 traitBits = (UInt32)TileGetFloatTrait(tile, kTrait_ListIndex);
		if (traitBits & kSyntheticFlag)
		{
			UInt32 id;
			if (!LookupSynthetic(tile, id))
				id = traitBits & 0xFFFFFF; //float precision loses low id bits, bookkeeping is authoritative

			if (g_eventManagerInterface && !g_isLoadingSave && !s_inDispatch)
			{
				void* speaker = GetSpeaker(dialogMenu);
				s_inDispatch = true;
				g_eventManagerInterface->DispatchEvent(kEventSelected, (TESObjectREFR*)speaker, speaker, (int)id);
				s_inDispatch = false;
			}

			//skip vanilla so the pending-topic global 0x11D9518 is never touched, keep current menu state
			//(the engine's own early-outs return meaningless eax values, so this is safe)
			return menuState;
		}
	}

	return s_origTopicClick ? s_origTopicClick(dialogMenu, edx, a3, tile) : 0;
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	if (!g_eventManagerInterface)
	{
		Log("OnDialogueMenuBuildHandler: event manager not ready, aborting Init");
		return false;
	}

	EnsureLock();
	EnsureRuleLock();

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;
	static P buildParams[] = { P::eParamType_AnyForm, P::eParamType_Int };
	static P selectParams[] = { P::eParamType_AnyForm, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent(kEventBuild, 2, buildParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent(kEventSelected, 2, selectParams, F::kFlag_FlushOnLoad);

	int installed = 0;

	if (s_buildDetour.WriteRelJump(kAddr_LoadTopicsList, (UInt32)Hook_LoadTopicsList, 9))
	{
		s_origLoadTopics = s_buildDetour.GetTrampoline<LoadTopicsList_t>();
		if (s_origLoadTopics) installed++;
		else { Log("OnDialogueMenuBuildHandler: LoadTopicsList trampoline missing"); s_buildDetour.Remove(); }
	}
	else Log("OnDialogueMenuBuildHandler: failed to hook LoadTopicsList");

	if (s_clickDetour.WriteRelJump(kAddr_TopicClick, (UInt32)Hook_TopicClick, 6))
	{
		s_origTopicClick = s_clickDetour.GetTrampoline<TopicClick_t>();
		if (s_origTopicClick) installed++;
		else { Log("OnDialogueMenuBuildHandler: topic click trampoline missing"); s_clickDetour.Remove(); }
	}
	else Log("OnDialogueMenuBuildHandler: failed to hook topic click");

	Log("OnDialogueMenuBuildHandler: %d/2 hooks installed", installed);
	return installed == 2;
}

}
