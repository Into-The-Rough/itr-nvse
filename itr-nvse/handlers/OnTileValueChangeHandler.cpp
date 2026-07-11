//watches named tile traits and fires ITR:OnTileValueChange when their value changes.
//two writer detours (TileValue::SetFloat/SetString) catch direct sets; the value teardown
//detour evicts a watched Value* the moment its tile is destroyed so the table never dangles.

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
static DWORD s_mainThreadId = 0;
static bool s_inDispatch = false;
static bool s_offThreadLogged = false;

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

static inline bool OnMainThread() { return GetCurrentThreadId() == s_mainThreadId; }

static void LogOffThreadOnce() {
	if (!s_offThreadLogged) {
		s_offThreadLogged = true;
		Log("OnTileValueChange: tile write off main thread, dropping (logged once)");
	}
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

static void DispatchChange(const WatchRecord& rec, float oldVal, float newVal) {
	if (!g_eventManagerInterface) return;
	if (!IsMenuLive(rec.menuID)) {          //menu closed, watched tiles gone, drop the stale watch
		if (EvictByValue(rec.value))
			Log("OnTileValueChange: dropped stale watch %d (menu %u closed)", rec.watchId, rec.menuID);
		return;
	}
	s_inDispatch = true;
	g_eventManagerInterface->DispatchEvent(kEventName, nullptr,
		(int)rec.menuID, (int)rec.traitID,
		PackEventFloatArg(oldVal), PackEventFloatArg(newVal), rec.watchId);
	s_inDispatch = false;
}

static void DispatchStrChange(const WatchRecord& rec, const char* oldStr, const char* newStr) {
	if (!g_eventManagerInterface) return;
	if (!IsMenuLive(rec.menuID)) {          //menu closed, watched tiles gone, drop the stale watch
		if (EvictByValue(rec.value))
			Log("OnTileValueChange: dropped stale watch %d (menu %u closed)", rec.watchId, rec.menuID);
		return;
	}
	s_inDispatch = true;
	g_eventManagerInterface->DispatchEvent(kStrEventName, nullptr,
		(int)rec.menuID, (int)rec.traitID,
		oldStr ? oldStr : "", newStr ? newStr : "", rec.watchId);
	s_inDispatch = false;
}

static int __fastcall Hook_SetFloat(void* thisV, void*, float value, char bPropagate) {
	if (s_watchCount == 0 || !thisV) return s_setFloat(thisV, value, bPropagate);
	if (!OnMainThread()) { LogOffThreadOnce(); return s_setFloat(thisV, value, bPropagate); }
	if (s_inDispatch) return s_setFloat(thisV, value, bPropagate);

	WatchRecord* r = FindRecord(thisV);
	if (!r) return s_setFloat(thisV, value, bPropagate);

	WatchRecord rec = *r;
	auto* v = static_cast<UIMinimal::Tile::Value*>(thisV);
	float oldNum = v->num;
	int ret = s_setFloat(thisV, value, bPropagate);
	float newNum = v->num;
	if (oldNum != newNum) DispatchChange(rec, oldNum, newNum);
	return ret;
}

static void* __fastcall Hook_SetString(void* thisV, void*, char* str, char bPropagate) {
	if (s_watchCount == 0 || !thisV) return s_setString(thisV, str, bPropagate);
	if (!OnMainThread()) { LogOffThreadOnce(); return s_setString(thisV, str, bPropagate); }
	if (s_inDispatch) return s_setString(thisV, str, bPropagate);

	WatchRecord* r = FindRecord(thisV);
	if (!r) return s_setString(thisV, str, bPropagate);

	WatchRecord rec = *r;
	auto* v = static_cast<UIMinimal::Tile::Value*>(thisV);
	char oldBuf[512];
	char* oldStr = v->str;   //0xC value string, the engine write frees and reallocates it
	if (oldStr) strncpy_s(oldBuf, oldStr, _TRUNCATE); else oldBuf[0] = '\0';
	void* ret = s_setString(thisV, str, bPropagate);
	char* newStr = v->str;
	//SetString always reallocs, so a pointer compare would fire on identical re-sets.
	//compare by content, matching the engine's own change flag
	const char* newCmp = newStr ? newStr : "";
	if (strcmp(oldBuf, newCmp) != 0) DispatchStrChange(rec, oldBuf, newStr);
	return ret;
}

static void* __fastcall Hook_UpdateReactions(void* thisV, void*, char bForce) {
	if (s_watchCount == 0 || !thisV) return s_updateReactions(thisV, bForce);
	if (!OnMainThread()) return s_updateReactions(thisV, bForce);
	if (s_inDispatch) return s_updateReactions(thisV, bForce);

	WatchRecord* r = FindRecord(thisV);
	if (!r) return s_updateReactions(thisV, bForce);

	WatchRecord rec = *r;
	auto* v = static_cast<UIMinimal::Tile::Value*>(thisV);
	float oldNum = v->num;
	void* ret = s_updateReactions(thisV, bForce);
	float newNum = v->num;
	if (oldNum != newNum) DispatchChange(rec, oldNum, newNum);   //reaction recompute moved num, a genuine change
	return ret;
}

static int __fastcall Hook_ValueTeardown(void* thisV, void*) {
	if (s_watchCount > 0 && thisV && OnMainThread()) {
		if (EvictByValue(thisV))
			Log("OnTileValueChange: evicted watch on destroyed value %p", thisV);
	}
	return s_teardown(thisV);
}

static bool Cmd_WatchTileValue_Execute(COMMAND_ARGS) {
	*result = 0;

	char pathBuf[512];
	char traitBuf[128];
	if (!ExtractArgs(EXTRACT_ARGS, &pathBuf, &traitBuf)) return true;

	UInt32 menuID = 0;
	void* tile = ResolvePathToTile(pathBuf, menuID);
	if (!tile) { Log("WatchTileValue: tile not found: %s", pathBuf); return true; }

	UInt32 traitID = CdeclCall<UInt32>(0xA00940 /*TraitNameToID*/, traitBuf, 0xFFFFFFFF /*auto assign*/);
	if (traitID == kTraitID_Invalid) { Log("WatchTileValue: bad trait '%s'", traitBuf); return true; }

	void* value = ThisCall<void*>(0xA01000 /*Tile::GetOrAddValue*/, tile, traitID);
	if (!value) { Log("WatchTileValue: no value for trait '%s' on %s", traitBuf, pathBuf); return true; }

	if (WatchRecord* existing = FindRecord(value)) { *result = existing->watchId; return true; }

	if (s_watchCount >= kMaxWatches) { Log("WatchTileValue: table full (%d)", kMaxWatches); return true; }

	int id = s_nextWatchId++;
	WatchRecord rec = { value, id, menuID, traitID };
	if (!Insert(rec)) { Log("WatchTileValue: insert failed"); return true; }
	s_watchCount++;
	*result = id;
	Log("WatchTileValue: watch %d on %s / %s (menu %u trait 0x%X)", id, pathBuf, traitBuf, menuID, traitID);
	return true;
}

static bool Cmd_UnwatchTileValue_Execute(COMMAND_ARGS) {
	*result = 0;

	UInt32 watchId = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &watchId)) return true;

	for (UInt32 i = 0; i < kTableSize; ++i) {
		if (s_table[i].value != nullptr && (UInt32)s_table[i].watchId == watchId) {
			RemoveAt(i);
			s_watchCount--;
			*result = 1;
			Log("UnwatchTileValue: removed watch %u", watchId);
			return true;
		}
	}
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

bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	s_mainThreadId = GetCurrentThreadId();

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

	int ok = 0;
	if (s_setFloatDetour.WriteRelJump(0xA0A270, Hook_SetFloat, 6)) {        //TileValue::SetFloat, 6-byte prologue
		s_setFloat = s_setFloatDetour.GetTrampoline<SetFloat_t>();
		if (s_setFloat) ok++; else s_setFloatDetour.Remove();
	} else Log("OnTileValueChange: failed to hook TileValue::SetFloat");

	if (s_setStringDetour.WriteRelJump(0xA0A300, Hook_SetString, 6)) {      //TileValue::SetString, 6-byte prologue
		s_setString = s_setStringDetour.GetTrampoline<SetString_t>();
		if (s_setString) ok++; else s_setStringDetour.Remove();
	} else Log("OnTileValueChange: failed to hook TileValue::SetString");

	if (s_teardownDetour.WriteRelJump(0xA09330, Hook_ValueTeardown, 6)) {   //TileValue teardown, destruction eviction, 6-byte prologue
		s_teardown = s_teardownDetour.GetTrampoline<ValueTeardown_t>();
		if (s_teardown) ok++; else s_teardownDetour.Remove();
	} else Log("OnTileValueChange: failed to hook TileValue teardown");

	if (s_reactionDetour.WriteRelJump(0xA09410, Hook_UpdateReactions, 5)) {   //TileValue::UpdateReactionsAndNotify, 5-byte prologue (push ebp/mov ebp,esp/push -1)
		s_updateReactions = s_reactionDetour.GetTrampoline<UpdateReactions_t>();
		if (s_updateReactions) ok++; else s_reactionDetour.Remove();
	} else Log("OnTileValueChange: failed to hook TileValue::UpdateReactionsAndNotify");

	Log("OnTileValueChange: %d/4 hooks installed", ok);
	return ok == 4;
}

void RegisterCommands(void* nvse) {
	NVSEInterface* n = (NVSEInterface*)nvse;
	n->RegisterCommand(&kCommandInfo_WatchTileValue);
	n->RegisterCommand(&kCommandInfo_UnwatchTileValue);
}

void ClearState() {
	for (UInt32 i = 0; i < kTableSize; ++i) s_table[i].value = nullptr;
	s_watchCount = 0;
	s_offThreadLogged = false;
}

}
