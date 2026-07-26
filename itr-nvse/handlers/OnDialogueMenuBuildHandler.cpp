//dialogue menu build event plus synthetic topic click routing
//build fires at DialogMenu::LoadTopicsList exit, on menu open and after each reply
//synthetic rows carry trait 4012 bit 0x40000000, which is out of range for the
//select-by-index guard sub_83E430, so a missed synthetic click is a vanilla no-op

#include "OnDialogueMenuBuildHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

class TESObjectREFR;

namespace OnDialogueMenuBuildHandler {

constexpr char kEventBuild[]    = "ITR:OnDialogueMenuBuild";
constexpr char kEventSelected[] = "ITR:OnDialogueTopicSelected";

constexpr UInt32 kSyntheticFlag = 0x40000000; //trait 4012 discriminator, out of sub_83E430 index range
constexpr int kTrait_ListIndex = 4012;

constexpr UInt32 kAddr_TopicClick     = 0x7624F0; //DialogMenu topic click handler

//0xA011B0 Tile::GetFloatTraitValue: __thiscall, ecx=tile, stack=traitID, returns float value as double
typedef double(__thiscall* GetFloatTrait_t)(void* tile, int traitID);
static const GetFloatTrait_t TileGetFloatTrait = (GetFloatTrait_t)0xA011B0;

typedef void(__thiscall* LoadTopicsList_t)(void* dialogMenu);
typedef unsigned int(__fastcall* TopicClick_t)(void* dialogMenu, void* edx, int a3, void* tile);

static Detours::CallDetour s_buildDetour;
static Detours::JumpDetour s_clickDetour;
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
constexpr int kMaxTopicRows = 96; //all or nothing, a longer chain is left untouched
constexpr int kNoOrder = 0x7FFFFFFF; //default sort key, keeps original relative order

struct OrderRule { UInt32 formID; int order; };
static UInt32 s_hideRules[kMaxHideRules];
static int s_hideCount = 0;
static OrderRule s_orderRules[kMaxOrderRules];
static int s_orderCount = 0;
static CRITICAL_SECTION s_ruleLock;
static volatile LONG s_ruleLockInit = 0;

static void EnsureRuleLock() { InitCriticalSectionOnce(&s_ruleLockInit, &s_ruleLock); }

//the chain as the engine last built it, sub_83EC30 reaches LoadTopicsList on three paths that leave
//the previous chain in place
static MenuTopicNodeView* s_vanillaHead = nullptr;
static MenuTopicNodeView* s_vanillaTerminator = nullptr;
static MenuTopicNodeView* s_vanillaRows[kMaxTopicRows];
static void* s_vanillaTopics[kMaxTopicRows];
static int s_vanillaCount = 0;

static void ClearVanillaOrder()
{
	s_vanillaHead = nullptr;
	s_vanillaTerminator = nullptr;
	s_vanillaCount = 0;
}

static UInt32 s_renderedRowCount = kRowCountUnknown;

UInt32 GetRenderedRowCount() { return s_renderedRowCount; }

//rows held out of the build, lives in the hook's frame so a nested build gets its own
struct TopicTransaction
{
	MenuTopicNodeView* head;         //greeting node, never moved
	MenuTopicNodeView* lastVisible;  //end of the rendered run, hidden rows go after it
	MenuTopicNodeView* terminator;
	MenuTopicNodeView* hidden[kMaxTopicRows];
	int hiddenCount;
	bool applied;
};

//assumes s_ruleLock held
static bool MenuTopicMatches(void* menuTopic, UInt32 formID)
{
	if (!menuTopic || !formID) return false;
	UInt32 infoID = FormViewGetRefID(MenuTopicGetTopicInfo(menuTopic));
	UInt32 topicID = FormViewGetRefID(MenuTopicGetTopic(menuTopic));
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
	ClearVanillaOrder();
	s_renderedRowCount = kRowCountUnknown;
}

//puts the chain back in engine order, only when the live nodes still match the snapshot exactly.
//compares pointer values and never dereferences a snapshot entry, so a stale one cannot fault
static bool RestoreVanillaOrder(MenuTopicNodeView* head)
{
	if (s_vanillaCount == 0 || head != s_vanillaHead) return false;

	int seen = 0;
	MenuTopicNodeView* cur = head->next;
	for (; cur && cur->menuTopic; cur = cur->next)
	{
		if (seen >= s_vanillaCount) return false;
		bool known = false;
		for (int i = 0; i < s_vanillaCount; i++)
		{
			if (s_vanillaRows[i] == cur && s_vanillaTopics[i] == cur->menuTopic) { known = true; break; }
		}
		if (!known) return false;
		seen++;
	}
	if (seen != s_vanillaCount) return false;
	if (cur != s_vanillaTerminator) return false;

	MenuTopicNodeView* prev = head;
	for (int i = 0; i < s_vanillaCount; i++) { prev->next = s_vanillaRows[i]; prev = s_vanillaRows[i]; }
	prev->next = s_vanillaTerminator;
	return true;
}

//cut hidden rows out of the chain and stable-sort the rest, LoadTopicsList renders every node it
//can reach so truncating is what hides a row. the greeting node never moves
static bool ApplyTopicRules(void* dialogMenu, TopicTransaction& tx)
{
	tx.applied = false;
	tx.hiddenCount = 0;

	EnsureRuleLock();
	ScopedLock lock(&s_ruleLock);

	const bool haveRules = (s_hideCount != 0 || s_orderCount != 0);
	if (!haveRules && s_vanillaCount == 0) return false;

	//DialogMenu+0x70 holds the manager pointer, the dword above it is the menu's BSSoundHandle
	void* manager = DialogMenuGetTopicManager(dialogMenu);
	if (!manager) return false;

	MenuTopicNodeView* head = MenuTopicManagerGetTopicList(manager);
	if (!head || !head->menuTopic) return false;

	RestoreVanillaOrder(head);
	if (!haveRules)
	{
		ClearVanillaOrder();
		return false;
	}

	MenuTopicNodeView* visible[kMaxTopicRows];
	int keys[kMaxTopicRows];
	int visCount = 0;

	s_vanillaCount = 0;
	s_vanillaHead = nullptr;

	MenuTopicNodeView* cur = head->next;
	int total = 0;
	while (cur && cur->menuTopic)
	{
		if (total >= kMaxTopicRows)
		{
			Log("ApplyTopicRules: chain exceeds %d rows, leaving it untouched", kMaxTopicRows);
			return false;
		}
		s_vanillaRows[total] = cur;
		s_vanillaTopics[total] = cur->menuTopic;
		total++;
		if (IsMenuTopicHidden(cur->menuTopic))
		{
			tx.hidden[tx.hiddenCount++] = cur;
		}
		else
		{
			visible[visCount] = cur;
			keys[visCount] = GetMenuTopicOrder(cur->menuTopic);
			visCount++;
		}
		cur = cur->next;
	}
	if (total == 0) return false;

	s_vanillaHead = head;
	s_vanillaTerminator = cur;
	s_vanillaCount = total;

	//stable insertion sort ascending by order, ties keep original sequence
	for (int i = 1; i < visCount; i++)
	{
		MenuTopicNodeView* node = visible[i];
		int key = keys[i];
		int j = i - 1;
		while (j >= 0 && keys[j] > key)
		{
			visible[j + 1] = visible[j];
			keys[j + 1] = keys[j];
			j--;
		}
		visible[j + 1] = node;
		keys[j + 1] = key;
	}

	MenuTopicNodeView* prev = head;
	for (int i = 0; i < visCount; i++) { prev->next = visible[i]; prev = visible[i]; }
	prev->next = cur;

	tx.head = head;
	tx.lastVisible = prev;
	tx.terminator = cur;
	tx.applied = true;

	if (visCount == 0)
		Log("ApplyTopicRules: every row hidden, the menu will draw topics empty");

	return true;
}

//relinks the hidden rows after the rendered run, never at their original positions: the click
//resolver sub_83E430 counts nodes from head->next, so the chain has to stay in rendered order while
//the rows are clickable. parked there they are still reachable for the engine's chain free
static void AppendHiddenRows(void* dialogMenu, TopicTransaction& tx)
{
	if (!tx.applied) return;
	tx.applied = false;
	if (tx.hiddenCount == 0) return;

	//a different head means something else rebuilt the chain and tx.hidden may already be freed
	void* manager = DialogMenuGetTopicManager(dialogMenu);
	if (!manager || MenuTopicManagerGetTopicList(manager) != tx.head)
	{
		Log("AppendHiddenRows: topic chain changed during the build, hidden rows left detached");
		return;
	}

	MenuTopicNodeView* prev = tx.lastVisible;
	for (int i = 0; i < tx.hiddenCount; i++) { prev->next = tx.hidden[i]; prev = tx.hidden[i]; }
	prev->next = tx.terminator;
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

static void __fastcall Hook_LoadTopicsList(void* dialogMenu, void* /*edx*/)
{
	TopicTransaction tx;
	tx.applied = false;
	tx.hiddenCount = 0;
	if (dialogMenu) ApplyTopicRules(dialogMenu, tx);

	LoadTopicsList_t orig = (LoadTopicsList_t)s_buildDetour.GetOverwrittenAddr();
	if (orig) orig(dialogMenu);

	if (dialogMenu) AppendHiddenRows(dialogMenu, tx);

	ClearState();

	//captured before handlers run, AddEntry bumps the same counter for every synthetic row
	s_renderedRowCount = dialogMenu ? DialogMenuGetTopicRowCount(dialogMenu) : kRowCountUnknown;

	if (g_eventManagerInterface && !g_isLoadingSave && dialogMenu && !s_inDispatch)
	{
		void* speaker = GetSpeaker(dialogMenu);
		UInt32 count = s_renderedRowCount;
		s_inDispatch = true;
		g_eventManagerInterface->DispatchEvent(kEventBuild, (TESObjectREFR*)speaker, speaker, (int)count);
		s_inDispatch = false;
	}
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

	if (s_buildDetour.WriteRelCall(0x76385E, (UInt32)Hook_LoadTopicsList))
		installed++;
	else
		Log("OnDialogueMenuBuildHandler: LoadTopicsList call site not hookable");

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
