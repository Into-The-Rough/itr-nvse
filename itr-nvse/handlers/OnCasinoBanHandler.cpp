//polls the player's CasinoDataList each frame to detect ban transitions.
//ban state is derived: earnings >= TESCasino.maxWinnings. tiny dataset (five
//casinos max in vanilla), negligible per-frame cost.

#include "OnCasinoBanHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"

static void** g_thePlayer = (void**)0x011DEA3C;
constexpr UInt32 kOffset_Player_CasinoDataList = 0x610;
constexpr UInt32 kOffset_Casino_MaxWinnings = 0x210;

struct SimpleListNode { void* item; SimpleListNode* next; };
struct CasinoStats { UInt32 casinoRefID; UInt32 earnings; UInt32 unk08; };

//bounded: five vanilla casinos. If a mod adds more, extras fall off the
//tail; worst case we miss edge events for mod-added casinos.
struct Tracked { UInt32 refID; bool banned; };
static Tracked g_tracked[16];
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
	if (g_trackedCount < 16) {
		g_tracked[g_trackedCount++] = { refID, banned };
	}
}

namespace OnCasinoBanHandler {
void Update()
{
	if (!g_eventManagerInterface) return;
	void* player = g_thePlayer ? *g_thePlayer : nullptr;
	if (!player) return;

	auto* list = *(SimpleListNode**)((UInt8*)player + kOffset_Player_CasinoDataList);

	for (SimpleListNode* n = list; n; n = n->next) {
		auto* entry = (CasinoStats*)n->item;
		if (!entry) continue;

		void* casino = Engine::LookupFormByID(entry->casinoRefID);
		if (!casino) continue;

		UInt32 max = *(UInt32*)((UInt8*)casino + kOffset_Casino_MaxWinnings);
		bool nowBanned = entry->earnings >= max;
		auto* tracked = GetTracked(entry->casinoRefID);
		if (!tracked) {
			SetLastState(entry->casinoRefID, nowBanned);
			continue;
		}

		if (nowBanned && !tracked->banned) {
			g_eventManagerInterface->DispatchEvent(
				"ITR:OnCasinoBan", nullptr, (TESForm*)casino);
		}
		tracked->banned = nowBanned;
	}
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
