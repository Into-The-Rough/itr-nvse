//owner name info handler - shows owner name on crosshair prompt
//modifies HUD tile text each frame after game updates it

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <Windows.h>

#include "OwnerNameInfoHandler.h"

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using SInt32 = int32_t;

//game addresses
constexpr UInt32 kAddr_HUDMainMenu = 0x11D96C0;
constexpr UInt32 kAddr_Player = 0x11DEA3C;
constexpr UInt32 kAddr_CrosshairRefName = 0x11D9C48;

//form type IDs
constexpr UInt8 kFormType_Faction = 0x08;
constexpr UInt8 kFormType_NPC = 0x2A;
constexpr UInt8 kFormType_Actor = 0x3B;

//extra data type IDs
constexpr UInt8 kExtraData_Ownership = 0x21;

//offsets
constexpr UInt32 kOffset_TESForm_TypeID = 0x04;
constexpr UInt32 kOffset_TESForm_RefID = 0x0C;
constexpr UInt32 kOffset_TESObjectREFR_ExtraDataList = 0x44;
constexpr UInt32 kOffset_TESObjectREFR_ParentCell = 0x40;
constexpr UInt32 kOffset_BaseExtraList_Data = 0x04;
constexpr UInt32 kOffset_BSExtraData_Type = 0x04;
constexpr UInt32 kOffset_BSExtraData_Next = 0x08;
constexpr UInt32 kOffset_ExtraOwnership_Owner = 0x0C;
constexpr UInt32 kOffset_TESObjectCELL_ExtraDataList = 0x28;
constexpr UInt32 kOffset_HUDMainMenu_CrosshairRef = 0x1B8;

//tile value IDs
constexpr UInt32 kTileValue_string = 0xFC4;

//Tile::SetStringValue at 0xA01350
typedef void (__thiscall* _TileSetString)(void* tile, UInt32 traitID, const char* str, bool propagate);
static const _TileSetString TileSetString = (_TileSetString)0xA01350;

//TESObjectREFR::IsInFaction at 0x575DB0 (returns rank or -1 if not in faction)
typedef SInt32 (__thiscall* _GetFactionRank)(void* refr, void* faction);
static const _GetFactionRank GetFactionRank = (_GetFactionRank)0x575DB0;

//settings
static bool g_bOwnerNameInfo = true;
static bool g_bCompatMode = true;
static bool g_bShowFactionName = true;
static bool g_bShowNameOnlyCrime = true;

static void* g_lastRef = nullptr;

//BSExtraData traversal
static void* GetExtraDataByType(void* extraDataList, UInt8 type)
{
	if (!extraDataList) return nullptr;
	void* data = *(void**)((UInt8*)extraDataList + kOffset_BaseExtraList_Data);
	int count = 0;
	while (data && count < 100)
	{
		UInt8 dataType = *(UInt8*)((UInt8*)data + kOffset_BSExtraData_Type);
		if (dataType == type)
			return data;
		data = *(void**)((UInt8*)data + kOffset_BSExtraData_Next);
		count++;
	}
	return nullptr;
}

//get owner from extra data list
static void* GetOwnerFromExtraList(void* extraDataList)
{
	void* xOwnership = GetExtraDataByType(extraDataList, kExtraData_Ownership);
	if (xOwnership)
		return *(void**)((UInt8*)xOwnership + kOffset_ExtraOwnership_Owner);
	return nullptr;
}

//get owner of reference (checks ref then cell)
static void* GetRefOwner(void* ref, bool* outIsFaction)
{
	if (!ref) return nullptr;
	*outIsFaction = false;

	void* extraDataList = (void*)((UInt8*)ref + kOffset_TESObjectREFR_ExtraDataList);
	void* owner = GetOwnerFromExtraList(extraDataList);

	if (!owner)
	{
		void* cell = *(void**)((UInt8*)ref + kOffset_TESObjectREFR_ParentCell);
		if (cell)
		{
			void* cellExtraList = (void*)((UInt8*)cell + kOffset_TESObjectCELL_ExtraDataList);
			owner = GetOwnerFromExtraList(cellExtraList);
		}
	}

	if (owner)
	{
		UInt8 ownerType = *(UInt8*)((UInt8*)owner + kOffset_TESForm_TypeID);
		*outIsFaction = (ownerType == kFormType_Faction);
	}

	return owner;
}

//get faction name - TESFaction::TESFullName at 0x18, String.data at +0x04
static const char* GetFactionName(void* faction)
{
	if (!faction) return nullptr;
	return *(const char**)((UInt8*)faction + 0x1C);
}

//get owner's name
static const char* GetOwnerName(void* owner)
{
	if (!owner) return nullptr;
	UInt8 typeID = *(UInt8*)((UInt8*)owner + kOffset_TESForm_TypeID);

	if (typeID == kFormType_Faction)
		return GetFactionName(owner);
	else if (typeID == kFormType_NPC)
		return *(const char**)((UInt8*)owner + 0xD4); //TESActorBase::TESFullName at 0xD0, String.data at +0x04

	return nullptr;
}

//string utilities
static bool EndsWithS(const char* str)
{
	size_t len = strlen(str);
	if (len == 0) return false;
	char last = str[len - 1];
	return last == 's' || last == 'S';
}

static bool StartsWithCaseInsensitive(const char* str, const char* prefix)
{
	while (*prefix)
	{
		if (tolower(*str++) != tolower(*prefix++))
			return false;
	}
	return true;
}

//clean up faction name (remove NV/DLC prefixes, add spaces to camelCase)
static void CleanFactionName(const char* input, char* output, size_t outputSize)
{
	if (!input || !output || outputSize == 0)
	{
		if (output && outputSize > 0) output[0] = '\0';
		return;
	}

	char temp[256];
	strncpy_s(temp, sizeof(temp), input, _TRUNCATE);

	char* src = temp;
	if (StartsWithCaseInsensitive(src, "NV"))
		src += 2;

	while (StartsWithCaseInsensitive(src, "DLC"))
	{
		src += 3;
		while (*src && isdigit(*src)) src++;
		while (*src && isspace(*src)) src++;
	}

	if (StartsWithCaseInsensitive(src, "Faction for "))
		src += 12;
	else if (StartsWithCaseInsensitive(src, "faction for "))
		src += 12;

	size_t len = strlen(src);
	while (len > 2 && src[len - 1] == '*' && src[len - 2] == ' ')
	{
		src[len - 2] = '\0';
		len -= 2;
	}

	//add spaces before uppercase in camelCase
	char result[256];
	size_t ri = 0;
	bool afterSpace = true;

	for (size_t i = 0; src[i] && ri < sizeof(result) - 2; i++)
	{
		char c = src[i];
		if (i > 0 && isupper(c) && islower(src[i - 1]) && ri < sizeof(result) - 3)
		{
			result[ri++] = ' ';
			afterSpace = true;
		}
		if (afterSpace && isalpha(c))
		{
			result[ri++] = toupper(c);
			afterSpace = false;
		}
		else
		{
			result[ri++] = c;
			afterSpace = isspace(c);
		}
	}
	result[ri] = '\0';

	char* start = result;
	while (*start && isspace(*start)) start++;
	char* end = start + strlen(start) - 1;
	while (end > start && isspace(*end)) *end-- = '\0';

	strncpy_s(output, outputSize, start, _TRUNCATE);
}

void ONI_Update()
{
	if (!g_bOwnerNameInfo)
		return;

	void* hud = *(void**)kAddr_HUDMainMenu;
	if (!hud)
		return;

	void* ref = *(void**)((UInt8*)hud + kOffset_HUDMainMenu_CrosshairRef);
	if (!ref)
	{
		g_lastRef = nullptr;
		return;
	}

	UInt8 refType = *(UInt8*)((UInt8*)ref + kOffset_TESForm_TypeID);

	//skip actors
	if (refType == kFormType_Actor)
	{
		g_lastRef = ref;
		return;
	}

	//get owner
	bool isFaction = false;
	void* owner = GetRefOwner(ref, &isFaction);
	if (!owner)
	{
		g_lastRef = ref;
		return;
	}

	const char* ownerName = GetOwnerName(owner);
	if (!ownerName || !*ownerName)
	{
		g_lastRef = ref;
		return;
	}

	//skip if owner is player (player NPC base form = 0x7)
	UInt32 ownerRefID = *(UInt32*)((UInt8*)owner + kOffset_TESForm_RefID);
	if (ownerRefID == 0x7)
	{
		g_lastRef = ref;
		return;
	}

	//skip if only showing for crimes and this isn't a crime
	if (g_bShowNameOnlyCrime && isFaction)
	{
		void* player = *(void**)kAddr_Player;
		if (player && GetFactionRank(player, owner) >= 0)
		{
			g_lastRef = ref;
			return;
		}
	}

	g_lastRef = ref;

	//get original item name from global buffer
	const char* itemName = (const char*)kAddr_CrosshairRefName;
	if (!itemName || !*itemName)
		return;

	//get tile for modification
	void** tiles = (void**)((UInt8*)hud + 0x2C);
	void* tile = tiles[31];
	if (!tile)
		return;

	//format modified name
	char modifiedName[512];
	if (isFaction)
	{
		if (!g_bShowFactionName)
			return;

		char cleanedFaction[256];
		CleanFactionName(ownerName, cleanedFaction, sizeof(cleanedFaction));

		if (g_bCompatMode)
			snprintf(modifiedName, sizeof(modifiedName), "%s (%s)", itemName, cleanedFaction);
		else
		{
			if (EndsWithS(cleanedFaction))
				snprintf(modifiedName, sizeof(modifiedName), "%s' %s", cleanedFaction, itemName);
			else
				snprintf(modifiedName, sizeof(modifiedName), "%s's %s", cleanedFaction, itemName);
		}
	}
	else
	{
		if (g_bCompatMode)
			snprintf(modifiedName, sizeof(modifiedName), "%s (%s)", itemName, ownerName);
		else
		{
			if (EndsWithS(ownerName))
				snprintf(modifiedName, sizeof(modifiedName), "%s' %s", ownerName, itemName);
			else
				snprintf(modifiedName, sizeof(modifiedName), "%s's %s", ownerName, itemName);
		}
	}

	TileSetString(tile, kTileValue_string, modifiedName, true);
}

bool ONI_Init()
{
	char iniPath[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("itr-nvse.dll"), iniPath, MAX_PATH);
	char* lastSlash = strrchr(iniPath, '\\');
	if (lastSlash) strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - iniPath), "itr-nvse.ini");

	g_bOwnerNameInfo = GetPrivateProfileIntA("Tweaks", "bOwnerNameInfo", 1, iniPath) != 0;
	g_bCompatMode = GetPrivateProfileIntA("OwnerNameInfo", "bCompatibilityMode", 1, iniPath) != 0;
	g_bShowFactionName = GetPrivateProfileIntA("OwnerNameInfo", "bShowFactionName", 1, iniPath) != 0;
	g_bShowNameOnlyCrime = GetPrivateProfileIntA("OwnerNameInfo", "bShowNameOnlyCrime", 1, iniPath) != 0;

	return true;
}
