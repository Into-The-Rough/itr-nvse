//polls the player's CasinoDataList for ban transitions. first sight of a
//casino seeds a baseline entry, the event only fires on a tracked
//unbanned-to-banned transition, so an entry we never saw unbanned stays silent.

#include "OnCasinoBanHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EngineFunctions.h"
#include "internal/GameSDK.h"
#include "internal/layout/Player.h"
#include "internal/GameGlobals.h"
#include "internal/EventDispatch.h"

struct SimpleListNode { void* item; SimpleListNode* next; };
struct CasinoStats { UInt32 casinoRefID; UInt32 earnings; UInt32 unk08; };

constexpr UInt32 kMaxTracked = 64;
struct Tracked { UInt32 refID; bool banned; };
static Tracked g_tracked[kMaxTracked];
static UInt32 g_trackedCount = 0;

static Tracked* GetTracked(UInt32 refID)
{
	for (UInt32 i = 0; i < g_trackedCount; ++i)
		if (g_tracked[i].refID == refID) return &g_tracked[i];
	return nullptr;
}

static void SetLastState(UInt32 refID, bool banned)
{
	if (auto* tracked = GetTracked(refID)) {
		tracked->banned = banned;
		return;
	}
	if (g_trackedCount < kMaxTracked) {
		g_tracked[g_trackedCount++] = { refID, banned };
	}
}

namespace OnCasinoBanHandler {
void Update()
{
	if (!g_eventManagerInterface) return;
	PlayerCharacter* player = *g_thePlayerPtr;
	if (!player) return;

	auto* list = reinterpret_cast<SimpleListNode*>(PlayerCharacterGetCasinoDataList(player));

	//an OnCasinoBan handler can run script that mutates the casino list, so record
	//transitions during the walk and dispatch once the list pointers are no longer live
	TESForm* newlyBanned[16];
	UInt32 newlyBannedCount = 0;

	for (SimpleListNode* n = list; n; n = n->next) {
		auto* entry = (CasinoStats*)n->item;
		if (!entry) continue;

		auto* casino = static_cast<TESForm*>(Engine::LookupFormByID(entry->casinoRefID));
		if (!casino) continue;

		UInt32 max = TESCasinoGetMaxWinnings(casino);
		bool nowBanned = entry->earnings >= max;
		auto* tracked = GetTracked(entry->casinoRefID);

		//untracked entries (first sight, or table overflow) seed baseline only,
		//dispatching for them would re-fire every frame while untracked
		bool crossedIntoBan = tracked && nowBanned && !tracked->banned;

		if (crossedIntoBan && newlyBannedCount < 16)
			newlyBanned[newlyBannedCount++] = casino;

		if (tracked)
			tracked->banned = nowBanned;
		else
			SetLastState(entry->casinoRefID, nowBanned);
	}

	for (UInt32 i = 0; i < newlyBannedCount; ++i)
		g_eventManagerInterface->DispatchEvent("ITR:OnCasinoBan", nullptr, newlyBanned[i]);
}

void ClearState()
{
	g_trackedCount = 0;
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;
	return true;
}
}
