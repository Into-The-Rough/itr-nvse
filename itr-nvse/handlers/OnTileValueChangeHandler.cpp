//queues watched Tile::Value writes for main-loop dispatch and evicts watched pointers on tile teardown

#include <Windows.h>
#include <cstring>

#include "OnTileValueChangeHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include "internal/CallTemplates.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

#define EXTRACT_ARGS paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList
using ExtractArgs_t = bool (*)(ParamInfo*, void*, UInt32*, TESObjectREFR*, TESObjectREFR*, Script*, ScriptEventList*, ...);
static ExtractArgs_t ExtractArgs = (ExtractArgs_t)0x5ACCB0;

namespace OnTileValueChangeHandler {

	constexpr char kEventName[] = "ITR:OnTileValueChange";
	constexpr char kStrEventName[] = "ITR:OnTileStrValueChange";
	constexpr UInt32 kTraitID_Invalid = 0x80000000; //TraitNameToID failure sentinel

	struct WatchRecord {
		void* value; //TileValue*, nullptr means empty slot
		int watchId;
		UInt32 menuID;
		UInt32 traitID;
	};

	constexpr UInt32 kTableSize = 64; //power of two, open addressing, linear probe
	constexpr UInt32 kTableMask = kTableSize - 1;
	constexpr int kMaxWatches = 48; //keep load factor below 0.75 for short probes

	static WatchRecord s_table[kTableSize] = {};
	static volatile int s_watchCount = 0;
	static int s_nextWatchId = 1;

	static CRITICAL_SECTION s_lock; //guards s_table, s_watchCount, s_queue
	static volatile LONG s_lockInit = 0;
	static bool s_hooksInstalled = false;

	static thread_local void* t_setFloatValue = nullptr; //SetFloat (0xA0A270) tail-calls UpdateReactions (0xA09410) on the same value, suppress only that value's inner capture so one write queues once, dependent tiles still report
	static thread_local bool t_inDispatch = false; //handler writes during drain are not recaptured

	struct PendingChange {
		void* value;
		int watchId;
		UInt32 menuID;
		UInt32 traitID;
		bool isString;
		float oldNum;
		float newNum;
		char oldStr[512];
		char newStr[512];
	};

	constexpr UInt32 kQueueSize = 32;
	static PendingChange s_queue[kQueueSize];
	static UInt32 s_queueCount = 0;

	static Detours::JumpDetour s_setFloatDetour;
	static Detours::JumpDetour s_setStringDetour;
	static Detours::JumpDetour s_teardownDetour;
	static Detours::JumpDetour s_reactionDetour;

	using SetFloat_t = int(__thiscall*)(void* thisV, float value, char bPropagate);
	using SetString_t = void*(__thiscall*)(void* thisV, char* str, char bPropagate);
	using ValueTeardown_t = int(__thiscall*)(void* thisV);
	using UpdateReactions_t = void*(__thiscall*)(void* thisV, char bForce);
	static SetFloat_t s_setFloat = nullptr;
	static SetString_t s_setString = nullptr;
	static ValueTeardown_t s_teardown = nullptr;
	static UpdateReactions_t s_updateReactions = nullptr;

	static inline UInt32 SlotHash(void* p)
	{
		return ((UInt32)p >> 4) & kTableMask; //value structs are heap aligned, drop dead low bits
	}

	//table functions below require the caller to hold s_lock
	static WatchRecord* FindRecord(void* value)
	{
		UInt32 i = SlotHash(value);
		for (UInt32 probes = 0; probes < kTableSize; ++probes) {
			if (s_table[i].value == value) return &s_table[i];
			if (s_table[i].value == nullptr) return nullptr;
			i = (i + 1) & kTableMask;
		}
		return nullptr;
	}

	static bool Insert(const WatchRecord& rec)
	{
		UInt32 i = SlotHash(rec.value);
		for (UInt32 probes = 0; probes < kTableSize; ++probes) {
			if (s_table[i].value == nullptr) {
				s_table[i] = rec;
				return true;
			}
			i = (i + 1) & kTableMask;
		}
		return false;
	}

	//clear slot j and rehash the trailing cluster so empty-slot probe termination stays valid
	static void RemoveAt(UInt32 j)
	{
		s_table[j].value = nullptr;
		UInt32 i = (j + 1) & kTableMask;
		while (s_table[i].value != nullptr) {
			WatchRecord moved = s_table[i];
			s_table[i].value = nullptr;
			Insert(moved);
			i = (i + 1) & kTableMask;
		}
	}

	static bool EvictByValue(void* value)
	{
		UInt32 i = SlotHash(value);
		for (UInt32 probes = 0; probes < kTableSize; ++probes) {
			if (s_table[i].value == value) {
				RemoveAt(i);
				s_watchCount--;
				return true;
			}
			if (s_table[i].value == nullptr) return false;
			i = (i + 1) & kTableMask;
		}
		return false;
	}

	static bool IsWatched(void* value)
	{
		if (s_lockInit != 2) return false;
		ScopedLock lock(&s_lock);
		return FindRecord(value) != nullptr;
	}

	static void QueueNumChange(void* value, float oldNum, float newNum)
	{
		if (s_lockInit != 2) return;
		ScopedLock lock(&s_lock);
		WatchRecord* r = FindRecord(value);
		if (!r || s_queueCount >= kQueueSize) return;
		PendingChange& p = s_queue[s_queueCount++];
		p.value = value;
		p.watchId = r->watchId;
		p.menuID = r->menuID;
		p.traitID = r->traitID;
		p.isString = false;
		p.oldNum = oldNum;
		p.newNum = newNum;
		p.oldStr[0] = '\0';
		p.newStr[0] = '\0';
	}

	static void QueueStrChange(void* value, const char* oldStr, const char* newStr)
	{
		if (s_lockInit != 2) return;
		ScopedLock lock(&s_lock);
		WatchRecord* r = FindRecord(value);
		if (!r || s_queueCount >= kQueueSize) return;
		PendingChange& p = s_queue[s_queueCount++];
		p.value = value;
		p.watchId = r->watchId;
		p.menuID = r->menuID;
		p.traitID = r->traitID;
		p.isString = true;
		p.oldNum = 0.0f;
		p.newNum = 0.0f;
		strncpy_s(p.oldStr, oldStr ? oldStr : "", _TRUNCATE);
		strncpy_s(p.newStr, newStr ? newStr : "", _TRUNCATE);
	}

	static UInt32 MenuIDFromRootTile(void* rootTile)
	{
		if (!rootTile) return 0;
		void* menu = *(void**)((char*)rootTile + 0x3C); //TileMenu::menu
		if (!menu) return 0;
		return *(UInt32*)((char*)menu + 0x20); //Menu::id
	}

	static bool IsMenuLive(UInt32 menuID)
	{
		TileMenuArrayView* arr = GetTileMenuArray();
		if (!arr) return false;
		for (UInt16 i = 0; i < arr->firstFreeEntry; ++i) {
			void* tm = arr->data[i];
			if (tm && MenuIDFromRootTile(tm) == menuID) return true;
		}
		return false;
	}

	static void* FindChildTile(void* parent, const char* name)
	{
		for (TileNodeView* node = TileGetFirstChild(parent); node; node = node->next) {
			const char* childName = TileGetName(node->data);
			if (childName && !_stricmp(childName, name)) return node->data;
		}
		return nullptr;
	}

	//resolves "menuname/child/child" to the target Tile and captures the owning menu id
	static void* ResolvePathToTile(const char* path, UInt32& menuIDOut)
	{
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
			if (tileName && !_stricmp(tileName, menuName)) {
				tile = tm;
				break;
			}
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

	static int __fastcall Hook_SetFloat(void* thisV, void* edx, float value, char bPropagate)
	{
		bool watched = s_watchCount != 0 && thisV && !t_inDispatch && IsWatched(thisV);
		float oldNum = watched ? static_cast<TileValueView*>(thisV)->num : 0.0f;
		void* prevSetFloat = t_setFloatValue;
		t_setFloatValue = thisV;
		int ret = s_setFloat(thisV, value, bPropagate);
		t_setFloatValue = prevSetFloat;
		if (watched) {
			float newNum = static_cast<TileValueView*>(thisV)->num;
			if (oldNum != newNum) QueueNumChange(thisV, oldNum, newNum);
		}
		return ret;
	}

	static void* __fastcall Hook_SetString(void* thisV, void* edx, char* str, char bPropagate)
	{
		if (s_watchCount == 0 || !thisV || t_inDispatch || !IsWatched(thisV))
			return s_setString(thisV, str, bPropagate);

		auto* v = static_cast<TileValueView*>(thisV);
		char oldBuf[512];
		char* oldStr = v->str; //0xC value string, the engine write frees and reallocates it
		if (oldStr) strncpy_s(oldBuf, oldStr, _TRUNCATE);
		else oldBuf[0] = '\0';
		void* ret = s_setString(thisV, str, bPropagate);
		//SetString always reallocs, so a pointer compare would fire on identical re-sets.
		//compare content over the stored bound, equal truncated prefixes count as unchanged
		const char* newCmp = v->str ? v->str : "";
		if (strncmp(oldBuf, newCmp, sizeof(oldBuf) - 1) != 0) QueueStrChange(thisV, oldBuf, newCmp);
		return ret;
	}

	static void* __fastcall Hook_UpdateReactions(void* thisV, void* edx, char bForce)
	{
		if (s_watchCount == 0 || !thisV || t_inDispatch || thisV == t_setFloatValue || !IsWatched(thisV))
			return s_updateReactions(thisV, bForce);

		auto* v = static_cast<TileValueView*>(thisV);
		float oldNum = v->num;
		void* ret = s_updateReactions(thisV, bForce);
		float newNum = v->num;
		if (oldNum != newNum) QueueNumChange(thisV, oldNum, newNum); //reaction recompute moved num, a genuine change
		return ret;
	}

	//tiles can be destroyed off the main thread, evict under the lock from any thread
	static int __fastcall Hook_ValueTeardown(void* thisV, void* edx)
	{
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

	void Update()
	{
		if (s_lockInit != 2 || !g_eventManagerInterface) return;
		if (s_queueCount == 0) return; //unlocked peek, a racing enqueue is picked up next frame

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
				WatchRecord* r = FindRecord(p.value);
				if (!r || r->watchId != p.watchId) continue; //watch evicted, or the value was rewatched under a new id since capture
			}
			if (!IsMenuLive(p.menuID)) { //menu closed, watched tiles gone, drop the stale watch
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

	static bool Cmd_WatchTileValue_Execute(COMMAND_ARGS)
	{
		*result = 0;

		char pathBuf[512];
		char traitBuf[128];
		if (!ExtractArgs(EXTRACT_ARGS, &pathBuf, &traitBuf)) return true;
		if (!s_hooksInstalled || s_lockInit != 2) return true;

		UInt32 menuID = 0;
		void* tile = ResolvePathToTile(pathBuf, menuID);
		if (!tile) {
			Log("WatchTileValue: tile not found: %s", pathBuf);
			return true;
		}

		UInt32 traitID = CdeclCall<UInt32>(0xA00940, traitBuf, 0xFFFFFFFF); //TraitNameToID, auto-assign
		if (traitID == kTraitID_Invalid) {
			Log("WatchTileValue: bad trait '%s'", traitBuf);
			return true;
		}

		void* value = ThisCall<void*>(0xA01000, tile, traitID); //Tile::GetOrCreateValue
		if (!value) {
			Log("WatchTileValue: no value for trait '%s' on %s", traitBuf, pathBuf);
			return true;
		}

		int id = 0;
		{
			ScopedLock lock(&s_lock);
			if (WatchRecord* existing = FindRecord(value)) {
				*result = existing->watchId;
				return true;
			}
			if (s_watchCount >= kMaxWatches) {
				Log("WatchTileValue: table full (%d)", kMaxWatches);
				return true;
			}
			id = s_nextWatchId++;
			WatchRecord rec = {value, id, menuID, traitID};
			if (!Insert(rec)) {
				Log("WatchTileValue: insert failed");
				return true;
			}
			s_watchCount++;
		}
		*result = id;
		Log("WatchTileValue: watch %d on %s / %s (menu %u trait 0x%X)", id, pathBuf, traitBuf, menuID, traitID);
		return true;
	}

	static bool Cmd_UnwatchTileValue_Execute(COMMAND_ARGS)
	{
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
		0, 2, kParams_WatchTileValue, Cmd_WatchTileValue_Execute, nullptr, nullptr, 0};

	static CommandInfo kCommandInfo_UnwatchTileValue = {
		"UnwatchTileValue", "", 0, "Stop watching a tile value by watch id",
		0, 1, kParams_UnwatchTileValue, Cmd_UnwatchTileValue_Execute, nullptr, nullptr, 0};

	bool Init(void* nvseInterface)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
		if (nvse->isEditor) return false;

		InitCriticalSectionOnce(&s_lockInit, &s_lock);

		if (g_eventManagerInterface) {
			using P = NVSEEventManagerInterface::ParamType;
			using F = NVSEEventManagerInterface::EventFlags;
			static P params[] = {
				P::eParamType_Int, P::eParamType_Int,
				P::eParamType_Float, P::eParamType_Float,
				P::eParamType_Int};
			g_eventManagerInterface->RegisterEvent(kEventName, 5, params, F::kFlag_FlushOnLoad);

			static P strParams[] = {
				P::eParamType_Int, P::eParamType_Int,
				P::eParamType_String, P::eParamType_String,
				P::eParamType_Int};
			g_eventManagerInterface->RegisterEvent(kStrEventName, 5, strParams, F::kFlag_FlushOnLoad);
		}

		s_hooksInstalled = s_setFloatDetour.WriteRelJump(0xA0A270, Hook_SetFloat, 6, reinterpret_cast<UInt8**>(&s_setFloat)) //TileValue::SetFloat
			&& s_setStringDetour.WriteRelJump(0xA0A300, Hook_SetString, 6, reinterpret_cast<UInt8**>(&s_setString)) //TileValue::SetString
			&& s_teardownDetour.WriteRelJump(0xA09330, Hook_ValueTeardown, 6, reinterpret_cast<UInt8**>(&s_teardown)) //TileValue teardown
			&& s_reactionDetour.WriteRelJump(0xA09410, Hook_UpdateReactions, 5, reinterpret_cast<UInt8**>(&s_updateReactions)); //TileValue::UpdateReactionsAndNotify
		return s_hooksInstalled;
	}

	void RegisterCommands(void* nvse)
	{
		NVSEInterface* n = (NVSEInterface*)nvse;
		n->RegisterCommand(&kCommandInfo_WatchTileValue);
		n->RegisterCommand(&kCommandInfo_UnwatchTileValue);
	}

	void ClearState()
	{
		if (s_lockInit != 2) return;
		ScopedLock lock(&s_lock);
		for (UInt32 i = 0; i < kTableSize; ++i) s_table[i].value = nullptr;
		s_watchCount = 0;
		s_queueCount = 0;
	}
}
