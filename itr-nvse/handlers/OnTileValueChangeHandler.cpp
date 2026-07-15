//watches named tile traits and fires ITR:OnTileValueChange when their value changes.
//writer detours (TileValue::SetFloat/SetString/UpdateReactions) capture plain values into
//a locked bounded queue and Update drains it on the main loop, so no script handler runs
//inside an engine tile write. the teardown detour evicts a watched Value* the moment its
//tile is destroyed so the table never dangles.

#include <Windows.h>
#include <cstring>

#include "OnTileValueChangeHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/UIMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

#define EXTRACT_ARGS paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList
typedef bool (*ExtractArgs_t)(ParamInfo*, void*, UInt32*, TESObjectREFR*, TESObjectREFR*, Script*, ScriptEventList*, ...);
static ExtractArgs_t ExtractArgs = (ExtractArgs_t)0x5ACCB0;

namespace OnTileValueChangeHandler {

constexpr char kEventName[] = "ITR:OnTileValueChange";
constexpr char kStrEventName[] = "ITR:OnTileStrValueChange";
constexpr UInt32 kTraitID_Invalid = 0x80000000;   //TraitNameToID failure sentinel

struct WatchRecord {
	void*  value;      //TileValue*, nullptr means empty slot
	int    watchId;
	UInt32 menuID;
	UInt32 traitID;
};

constexpr UInt32 kTableSize = 64;             //power of two, open addressing, linear probe
constexpr UInt32 kTableMask = kTableSize - 1;
constexpr int    kMaxWatches = 48;            //keep load factor below 0.75 for short probes

static WatchRecord s_table[kTableSize] = {};
static volatile int s_watchCount = 0;
static int s_nextWatchId = 1;

static CRITICAL_SECTION s_lock;               //guards s_table, s_watchCount, s_queue
static volatile LONG s_lockInit = 0;

static thread_local int t_inSetFloat = 0;     //SetFloat (0xA0A270) tail-calls UpdateReactions (0xA09410), suppress the inner capture so one write queues once
static thread_local bool t_inDispatch = false; //handler writes during drain are not recaptured

struct PendingChange {
	void*  value;
	int    watchId;
	UInt32 menuID;
	UInt32 traitID;
	bool   isString;
	float  oldNum;
	float  newNum;
	char   oldStr[512];
	char   newStr[512];
};

constexpr UInt32 kQueueSize = 32;
static PendingChange s_queue[kQueueSize];
static UInt32 s_queueCount = 0;

static Detours::JumpDetour s_setFloatDetour;
static Detours::JumpDetour s_setStringDetour;
static Detours::JumpDetour s_teardownDetour;
static Detours::JumpDetour s_reactionDetour;

typedef int   (__thiscall* SetFloat_t)(void* thisV, float value, char bPropagate);
typedef void* (__thiscall* SetString_t)(void* thisV, char* str, char bPropagate);
typedef int   (__thiscall* ValueTeardown_t)(void* thisV);
typedef void* (__thiscall* UpdateReactions_t)(void* thisV, char bForce);
static SetFloat_t         s_setFloat = nullptr;
static SetString_t        s_setString = nullptr;
static ValueTeardown_t    s_teardown = nullptr;
static UpdateReactions_t  s_updateReactions = nullptr;

static inline UInt32 SlotHash(void* p) {
	return ((UInt32)(uintptr_t)p >> 4) & kTableMask;   //value structs are heap aligned, drop dead low bits
}

//table functions below require the caller to hold s_lock
static WatchRecord* FindRecord(void* value) {
	UInt32 i = SlotHash(value);
	for (UInt32 probes = 0; probes < kTableSize; ++probes) {
		if (s_table[i].value == value) return &s_table[i];
		if (s_table[i].value == nullptr) return nullptr;
		i = (i + 1) & kTableMask;
	}
	return nullptr;
}

static bool Insert(const WatchRecord& rec) {
	UInt32 i = SlotHash(rec.value);
	for (UInt32 probes = 0; probes < kTableSize; ++probes) {
		if (s_table[i].value == nullptr) { s_table[i] = rec; return true; }
		i = (i + 1) & kTableMask;
	}
	return false;
}

//clear slot j and rehash the trailing cluster so empty-slot probe termination stays valid
static void RemoveAt(UInt32 j) {
	s_table[j].value = nullptr;
	UInt32 i = (j + 1) & kTableMask;
	while (s_table[i].value != nullptr) {
		WatchRecord moved = s_table[i];
		s_table[i].value = nullptr;
		Insert(moved);
		i = (i + 1) & kTableMask;
	}
}

static bool EvictByValue(void* value) {
	UInt32 i = SlotHash(value);
	for (UInt32 probes = 0; probes < kTableSize; ++probes) {
		if (s_table[i].value == value) { RemoveAt(i); s_watchCount--; return true; }
		if (s_table[i].value == nullptr) return false;
		i = (i + 1) & kTableMask;
	}
	return false;
}

static bool IsWatched(void* value) {
	if (s_lockInit != 2) return false;
	ScopedLock lock(&s_lock);
	return FindRecord(value) != nullptr;
}

static void QueueNumChange(void* value, float oldNum, float newNum) {
	if (s_lockInit != 2) return;
	ScopedLock lock(&s_lock);
	WatchRecord* r = FindRecord(value);
	if (!r || s_queueCount >= kQueueSize) return;
	PendingChange& p = s_queue[s_queueCount++];
	p.value = value; p.watchId = r->watchId; p.menuID = r->menuID; p.traitID = r->traitID;
	p.isString = false;
	p.oldNum = oldNum; p.newNum = newNum;
	p.oldStr[0] = '\0'; p.newStr[0] = '\0';
}

static void QueueStrChange(void* value, const char* oldStr, const char* newStr) {
	if (s_lockInit != 2) return;
	ScopedLock lock(&s_lock);
	WatchRecord* r = FindRecord(value);
	if (!r || s_queueCount >= kQueueSize) return;
	PendingChange& p = s_queue[s_queueCount++];
	p.value = value; p.watchId = r->watchId; p.menuID = r->menuID; p.traitID = r->traitID;
	p.isString = true;
	p.oldNum = 0.0f; p.newNum = 0.0f;
	strncpy_s(p.oldStr, oldStr ? oldStr : "", _TRUNCATE);
	strncpy_s(p.newStr, newStr ? newStr : "", _TRUNCATE);
}

static UInt32 MenuIDFromRootTile(void* rootTile) {
	if (!rootTile) return 0;
	void* menu = *(void**)((char*)rootTile + 0x3C);   //TileMenu::menu
	if (!menu) return 0;
	return *(UInt32*)((char*)menu + 0x20);            //Menu::id
}

static bool IsMenuLive(UInt32 menuID) {
	TileMenuArrayView* arr = GetTileMenuArray();
	if (!arr) return false;
	for (UInt16 i = 0; i < arr->firstFreeEntry; ++i) {
		void* tm = arr->data[i];
		if (tm && MenuIDFromRootTile(tm) == menuID) return true;
	}
	return false;
}

static void* FindChildTile(void* parent, const char* name) {
	for (TileNodeView* node = TileGetFirstChild(parent); node; node = node->next) {
		const char* childName = TileGetName(node->data);
		if (childName && !_stricmp(childName, name)) return node->data;
	}
	return nullptr;
}

//resolves "menuname/child/child" to the target Tile and captures the owning menu id
static void* ResolvePathToTile(const char* path, UInt32& menuIDOut) {
	menuIDOut = 0;
	char buf[512];
	strncpy_s(buf, path, _TRUNCATE);

	char* ctx = nullptr;
	char* menuName = strtok_s(buf, "\\/", &ctx);
	if (!menuName) return nullptr;

	void* tile = nullptr;
	TileMenuArrayView* arr = GetTileMenuArray();
	if (!arr) return nullptr;
	for (UInt16 i = 0; i < arr->firstFreeEntry; ++i) {
		void* tm = arr->data[i];
		const char* tileName = TileGetName(tm);
		if (tileName && !_stricmp(tileName, menuName)) { tile = tm; break; }
	}
	if (!tile) return nullptr;

	menuIDOut = MenuIDFromRootTile(tile);

	char* segment = nullptr;
	while ((segment = strtok_s(nullptr, "\\/", &ctx)) != nullptr) {
		tile = FindChildTile(tile, segment);
		if (!tile) return nullptr;
	}
	return tile;
}

static int __fastcall Hook_SetFloat(void* thisV, void*, float value, char bPropagate) {
	bool watched = s_watchCount != 0 && thisV && !t_inDispatch && IsWatched(thisV);
	float oldNum = watched ? static_cast<UIMinimal::Tile::Value*>(thisV)->num : 0.0f;
	t_inSetFloat++;
	int ret = s_setFloat(thisV, value, bPropagate);
	t_inSetFloat--;
	if (watched) {
		float newNum = static_cast<UIMinimal::Tile::Value*>(thisV)->num;
		if (oldNum != newNum) QueueNumChange(thisV, oldNum, newNum);
	}
	return ret;
}

static void* __fastcall Hook_SetString(void* thisV, void*, char* str, char bPropagate) {
	if (s_watchCount == 0 || !thisV || t_inDispatch || !IsWatched(thisV))
		return s_setString(thisV, str, bPropagate);

	auto* v = static_cast<UIMinimal::Tile::Value*>(thisV);
	char oldBuf[512];
	char* oldStr = v->str;   //0xC value string, the engine write frees and reallocates it
	if (oldStr) strncpy_s(oldBuf, oldStr, _TRUNCATE); else oldBuf[0] = '\0';
	void* ret = s_setString(thisV, str, bPropagate);
	//SetString always reallocs, so a pointer compare would fire on identical re-sets.
	//compare content over the stored bound, equal truncated prefixes count as unchanged
	const char* newCmp = v->str ? v->str : "";
	if (strncmp(oldBuf, newCmp, sizeof(oldBuf) - 1) != 0) QueueStrChange(thisV, oldBuf, newCmp);
	return ret;
}

static void* __fastcall Hook_UpdateReactions(void* thisV, void*, char bForce) {
	if (s_watchCount == 0 || !thisV || t_inDispatch || t_inSetFloat || !IsWatched(thisV))
		return s_updateReactions(thisV, bForce);

	auto* v = static_cast<UIMinimal::Tile::Value*>(thisV);
	float oldNum = v->num;
	void* ret = s_updateReactions(thisV, bForce);
	float newNum = v->num;
	if (oldNum != newNum) QueueNumChange(thisV, oldNum, newNum);   //reaction recompute moved num, a genuine change
	return ret;
}

//tiles can be destroyed off the main thread, evict under the lock from any thread
static int __fastcall Hook_ValueTeardown(void* thisV, void*) {
	if (thisV && s_lockInit == 2) {
		bool evicted;
		{
			ScopedLock lock(&s_lock);
			evicted = s_watchCount > 0 && EvictByValue(thisV);
		}
		if (evicted) Log("OnTileValueChange: evicted watch on destroyed value %p", thisV);
	}
	return s_teardown(thisV);
}

void Update() {
	if (s_lockInit != 2 || !g_eventManagerInterface) return;
	if (s_queueCount == 0) return;   //unlocked peek, a racing enqueue is picked up next frame

	static PendingChange s_drain[kQueueSize];
	UInt32 count;
	{
		ScopedLock lock(&s_lock);
		count = s_queueCount;
		memcpy(s_drain, s_queue, count * sizeof(PendingChange));
		s_queueCount = 0;
	}

	t_inDispatch = true;
	for (UInt32 i = 0; i < count; ++i) {
		PendingChange& p = s_drain[i];
		{
			ScopedLock lock(&s_lock);
			if (!FindRecord(p.value)) continue;   //watch evicted since capture, tile is gone
		}
		if (!IsMenuLive(p.menuID)) {              //menu closed, watched tiles gone, drop the stale watch
			bool evicted;
			{
				ScopedLock lock(&s_lock);
				evicted = EvictByValue(p.value);
			}
			if (evicted) Log("OnTileValueChange: dropped stale watch %d (menu %u closed)", p.watchId, p.menuID);
			continue;
		}
		if (p.isString)
			g_eventManagerInterface->DispatchEvent(kStrEventName, nullptr,
				(int)p.menuID, (int)p.traitID, p.oldStr, p.newStr, p.watchId);
		else
			g_eventManagerInterface->DispatchEvent(kEventName, nullptr,
				(int)p.menuID, (int)p.traitID,
				PackEventFloatArg(p.oldNum), PackEventFloatArg(p.newNum), p.watchId);
	}
	t_inDispatch = false;
}

static bool Cmd_WatchTileValue_Execute(COMMAND_ARGS) {
	*result = 0;

	char pathBuf[512];
	char traitBuf[128];
	if (!ExtractArgs(EXTRACT_ARGS, &pathBuf, &traitBuf)) return true;
	if (s_lockInit != 2) return true;

	UInt32 menuID = 0;
	void* tile = ResolvePathToTile(pathBuf, menuID);
	if (!tile) { Log("WatchTileValue: tile not found: %s", pathBuf); return true; }

	UInt32 traitID = CdeclCall<UInt32>(0xA00940 /*TraitNameToID*/, traitBuf, 0xFFFFFFFF /*auto assign*/);
	if (traitID == kTraitID_Invalid) { Log("WatchTileValue: bad trait '%s'", traitBuf); return true; }

	void* value = ThisCall<void*>(0xA01000 /*Tile::GetOrAddValue*/, tile, traitID);
	if (!value) { Log("WatchTileValue: no value for trait '%s' on %s", traitBuf, pathBuf); return true; }

	int id = 0;
	{
		ScopedLock lock(&s_lock);
		if (WatchRecord* existing = FindRecord(value)) { *result = existing->watchId; return true; }
		if (s_watchCount >= kMaxWatches) { Log("WatchTileValue: table full (%d)", kMaxWatches); return true; }
		id = s_nextWatchId++;
		WatchRecord rec = { value, id, menuID, traitID };
		if (!Insert(rec)) { Log("WatchTileValue: insert failed"); return true; }
		s_watchCount++;
	}
	*result = id;
	Log("WatchTileValue: watch %d on %s / %s (menu %u trait 0x%X)", id, pathBuf, traitBuf, menuID, traitID);
	return true;
}

static bool Cmd_UnwatchTileValue_Execute(COMMAND_ARGS) {
	*result = 0;

	UInt32 watchId = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &watchId)) return true;
	if (s_lockInit != 2) return true;

	{
		ScopedLock lock(&s_lock);
		for (UInt32 i = 0; i < kTableSize; ++i) {
			if (s_table[i].value != nullptr && (UInt32)s_table[i].watchId == watchId) {
				RemoveAt(i);
				s_watchCount--;
				*result = 1;
				break;
			}
		}
	}
	if (*result != 0) Log("UnwatchTileValue: removed watch %u", watchId);
	return true;
}

static ParamInfo kParams_WatchTileValue[2] = {
	{"tilePath", kParamType_String, 0},
	{"traitName", kParamType_String, 0},
};

static ParamInfo kParams_UnwatchTileValue[1] = {
	{"watchId", kParamType_Integer, 0},
};

static CommandInfo kCommandInfo_WatchTileValue = {
	"WatchTileValue", "", 0, "Watch a UI tile trait for value changes, returns a watch id or 0",
	0, 2, kParams_WatchTileValue, Cmd_WatchTileValue_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_UnwatchTileValue = {
	"UnwatchTileValue", "", 0, "Stop watching a tile value by watch id",
	0, 1, kParams_UnwatchTileValue, Cmd_UnwatchTileValue_Execute, nullptr, nullptr, 0
};

static void RemoveHooks() {
	if (s_setFloatDetour.IsInstalled()) s_setFloatDetour.Remove();
	if (s_setStringDetour.IsInstalled()) s_setStringDetour.Remove();
	if (s_teardownDetour.IsInstalled()) s_teardownDetour.Remove();
	if (s_reactionDetour.IsInstalled()) s_reactionDetour.Remove();
	s_setFloat = nullptr;
	s_setString = nullptr;
	s_teardown = nullptr;
	s_updateReactions = nullptr;
}

bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	InitCriticalSectionOnce(&s_lockInit, &s_lock);

	if (g_eventManagerInterface) {
		using P = NVSEEventManagerInterface::ParamType;
		using F = NVSEEventManagerInterface::EventFlags;
		static P params[] = {
			P::eParamType_Int, P::eParamType_Int,
			P::eParamType_Float, P::eParamType_Float,
			P::eParamType_Int
		};
		g_eventManagerInterface->RegisterEvent(kEventName, 5, params, F::kFlag_FlushOnLoad);

		static P strParams[] = {
			P::eParamType_Int, P::eParamType_Int,
			P::eParamType_String, P::eParamType_String,
			P::eParamType_Int
		};
		g_eventManagerInterface->RegisterEvent(kStrEventName, 5, strParams, F::kFlag_FlushOnLoad);
	} else {
		Log("OnTileValueChange: event manager not ready at Init");
	}

	bool ok = true;
	if (ok && s_setFloatDetour.WriteRelJump(0xA0A270, Hook_SetFloat, 6))         //TileValue::SetFloat, 6-byte prologue
		s_setFloat = s_setFloatDetour.GetTrampoline<SetFloat_t>();
	ok = ok && s_setFloat != nullptr;

	if (ok && s_setStringDetour.WriteRelJump(0xA0A300, Hook_SetString, 6))       //TileValue::SetString, 6-byte prologue
		s_setString = s_setStringDetour.GetTrampoline<SetString_t>();
	ok = ok && s_setString != nullptr;

	if (ok && s_teardownDetour.WriteRelJump(0xA09330, Hook_ValueTeardown, 6))    //TileValue teardown, destruction eviction, 6-byte prologue
		s_teardown = s_teardownDetour.GetTrampoline<ValueTeardown_t>();
	ok = ok && s_teardown != nullptr;

	if (ok && s_reactionDetour.WriteRelJump(0xA09410, Hook_UpdateReactions, 5))  //TileValue::UpdateReactionsAndNotify, 5-byte prologue (push ebp/mov ebp,esp/push -1)
		s_updateReactions = s_reactionDetour.GetTrampoline<UpdateReactions_t>();
	ok = ok && s_updateReactions != nullptr;

	if (!ok) {
		//a half-hooked tile system must not accept watches, roll back and fail Init
		RemoveHooks();
		Log("OnTileValueChange: hook install failed, feature disabled");
		return false;
	}

	Log("OnTileValueChange: 4/4 hooks installed");
	return true;
}

void RegisterCommands(void* nvse) {
	NVSEInterface* n = (NVSEInterface*)nvse;
	n->RegisterCommand(&kCommandInfo_WatchTileValue);
	n->RegisterCommand(&kCommandInfo_UnwatchTileValue);
}

void ClearState() {
	if (s_lockInit != 2) return;
	ScopedLock lock(&s_lock);
	for (UInt32 i = 0; i < kTableSize; ++i) s_table[i].value = nullptr;
	s_watchCount = 0;
	s_queueCount = 0;
}

}
