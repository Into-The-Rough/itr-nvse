#include "CasinoBanCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

//per-casino ban toggle. ban state is derived: banned = CasinoStats.earnings >= TESCasino.maxWinnings.
//no engine patches - vanilla quest scripts (vCasinoCompsX) keep driving the floor-manager ambush via
//GetCasinoWinningsLevel, which is what YUP's dialogue edits hang off.

constexpr UInt32 kFormType_TESCasino = 0x6D;

//TESCasino
constexpr UInt32 kOffset_Casino_MaxWinnings = 0x210;

//PlayerCharacter
constexpr UInt32 kOffset_Player_CasinoDataList = 0x610;
static void* const kAddr_PlayerSingleton = (void*)0x11DEA3C;

//engine funcs we borrow from BlackJackMenu::Create
typedef void* (__cdecl*  _OperatorNew)(UInt32 size);
typedef void (__thiscall* _CasinoStatsCtor)(void* self);
typedef bool (__thiscall* _BSSimpleList_IsEmpty)(void* self);
typedef void (__thiscall* _BSSimpleList_AddHead)(void* self, void** item);
typedef void (__thiscall* _BSSimpleList_SetItem0)(void* self, void** item);

static const _OperatorNew           OperatorNew          = (_OperatorNew)0x401000;
static const _CasinoStatsCtor       CasinoStatsCtor      = (_CasinoStatsCtor)0x733F50;
static const _BSSimpleList_IsEmpty  BSSimpleList_IsEmpty = (_BSSimpleList_IsEmpty)0x8256D0;
static const _BSSimpleList_AddHead  BSSimpleList_AddHead = (_BSSimpleList_AddHead)0x5AE3D0;
static const _BSSimpleList_SetItem0 BSSimpleList_SetItem0 = (_BSSimpleList_SetItem0)0x726C60;

struct CasinoStats { UInt32 casinoRefID; UInt32 earnings; UInt32 unk08; };

struct SimpleListNode { void* item; SimpleListNode* next; };

static SimpleListNode* GetCasinoDataList()
{
	void* player = *(void**)kAddr_PlayerSingleton;
	if (!player) return nullptr;
	return *(SimpleListNode**)((UInt8*)player + kOffset_Player_CasinoDataList);
}

static CasinoStats* FindEntry(SimpleListNode* list, UInt32 refID)
{
	for (SimpleListNode* n = list; n; n = n->next) {
		auto* entry = (CasinoStats*)n->item;
		if (entry && entry->casinoRefID == refID) return entry;
	}
	return nullptr;
}

static CasinoStats* CreateEntry(SimpleListNode* list, UInt32 refID)
{
	auto* entry = (CasinoStats*)OperatorNew(sizeof(CasinoStats));
	if (!entry) return nullptr;
	CasinoStatsCtor(entry);
	entry->casinoRefID = refID;
	entry->earnings = 0;

	void* item = entry;
	if (BSSimpleList_IsEmpty(list))
		BSSimpleList_SetItem0(list, &item);
	else
		BSSimpleList_AddHead(list, &item);
	return entry;
}

static UInt32 GetCasinoRefID(TESForm* casino) {
	typedef UInt32 (__thiscall* _GetRefID)(void*);
	auto fn = (_GetRefID)(*(void***)casino)[3];
	return fn(casino);
}

static ParamInfo kParams_SetCasinoBan[2] = {
	{"casino", kParamType_AnyForm, 0},
	{"banned", kParamType_Integer, 0},
};

static ParamInfo kParams_GetCasinoBan[1] = {
	{"casino", kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(SetCasinoBan, "Set per-casino ban state on the player", 0, 2, kParams_SetCasinoBan);
DEFINE_COMMAND_PLUGIN(GetCasinoBan, "Get per-casino ban state for the player", 0, 1, kParams_GetCasinoBan);

bool Cmd_SetCasinoBan_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* casino = nullptr;
	UInt32 banned = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &casino, &banned)) return true;
	if (!casino) return true;
	if (*((UInt8*)casino + 0x04) != kFormType_TESCasino) return true;

	SimpleListNode* list = GetCasinoDataList();
	if (!list) return true;

	UInt32 refID = GetCasinoRefID(casino);
	UInt32 maxWinnings = *(UInt32*)((UInt8*)casino + kOffset_Casino_MaxWinnings);

	CasinoStats* entry = FindEntry(list, refID);
	if (banned) {
		if (!entry) entry = CreateEntry(list, refID);
		if (!entry) return true;
		entry->earnings = maxWinnings;
	} else {
		if (!entry) { *result = 1; return true; } //already "not banned"
		if (entry->earnings >= maxWinnings)
			entry->earnings = maxWinnings > 0 ? maxWinnings - 1 : 0;
	}
	*result = 1;
	return true;
}

bool Cmd_GetCasinoBan_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* casino = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &casino)) return true;
	if (!casino) return true;
	if (*((UInt8*)casino + 0x04) != kFormType_TESCasino) return true;

	SimpleListNode* list = GetCasinoDataList();
	if (!list) return true;

	CasinoStats* entry = FindEntry(list, GetCasinoRefID(casino));
	if (!entry) return true;

	UInt32 maxWinnings = *(UInt32*)((UInt8*)casino + kOffset_Casino_MaxWinnings);
	*result = (entry->earnings >= maxWinnings) ? 1.0 : 0.0;
	return true;
}

namespace CasinoBanCommands {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetCasinoBan);
	nvse->RegisterCommand(&kCommandInfo_GetCasinoBan);
}
}
