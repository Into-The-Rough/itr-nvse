//Rewrites the crosshair prompt with owner information after the HUD updates it.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <Windows.h>

#include "OwnerNameInfoHandler.h"
#include "internal/EngineFunctions.h"


constexpr UInt8 kFormType_Faction = 0x08;
constexpr UInt32 kOffset_TESForm_TypeID = 0x04;

//TESObjectREFR::IsCrime at 0x579690 (checks if activating/taking would be a crime)
typedef bool (__thiscall* _IsCrime)(void* refr);
static const _IsCrime IsCrime = (_IsCrime)0x579690;

static bool g_bOwnerNameInfo = false;
static bool g_bCompatMode = true;
static bool g_bShowFactionName = true;
static bool g_bShowNameOnlyCrime = true;

static void* g_lastRef = nullptr;

static void* GetRefOwner(void* ref, bool* outIsFaction)
{
	if (!ref) return nullptr;
	*outIsFaction = false;

	// Keep prompt ownership aligned with the engine's crime/ownership logic.
	void* owner = Engine::TESObjectREFR_GetOwnerRawForm(ref);

	if (owner)
	{
		UInt8 ownerType = *(UInt8*)((UInt8*)owner + kOffset_TESForm_TypeID);
		*outIsFaction = (ownerType == kFormType_Faction);
	}

	return owner;
}

static const char* GetFormEditorID(void* form)
{
	if (!form) return nullptr;
	void** vtable = *(void***)form;
	if (!vtable) return nullptr;
	typedef const char* (__thiscall* GetEditorIDFn)(void*);
	auto fn = (GetEditorIDFn)vtable[0x4C];
	return fn ? fn(form) : nullptr;
}

static const char* GetFactionName(void* faction)
{
	if (!faction) return nullptr;
	const char* name = *(const char**)((UInt8*)faction + 0x1C);
	if (name && *name)
		return name;
	return GetFormEditorID(faction);
}

static const char* GetOwnerName(void* owner)
{
	if (!owner) return nullptr;
	UInt8 typeID = *(UInt8*)((UInt8*)owner + kOffset_TESForm_TypeID);

	if (typeID == kFormType_Faction)
		return GetFactionName(owner);
	else if (typeID == 0x2A) //NPC
	{
		const char* name = *(const char**)((UInt8*)owner + 0xD4); //TESActorBase::TESFullName at 0xD0, String.data at +0x04
		if (name && *name)
			return name;
		return GetFormEditorID(owner);
	}

	return nullptr;
}

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
	size_t slen = strlen(start);
	if (slen > 0)
	{
		char* end = start + slen - 1;
		while (end > start && isspace(*end)) *end-- = '\0';
	}

	strncpy_s(output, outputSize, start, _TRUNCATE);
}

namespace OwnerNameInfoHandler {
void Update()
{
	if (!g_bOwnerNameInfo)
		return;

	void* hud = *(void**)0x11D96C0; //HUDMainMenu
	if (!hud)
		return;

	void* ref = *(void**)((UInt8*)hud + 0x1B8); //crosshairRef
	if (!ref)
	{
		g_lastRef = nullptr;
		return;
	}

	UInt8 refType = *(UInt8*)((UInt8*)ref + kOffset_TESForm_TypeID);

	if (refType == 0x3B) //Actor
	{
		g_lastRef = ref;
		return;
	}

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

	UInt32 ownerRefID = *(UInt32*)((UInt8*)owner + 0x0C); //TESForm::refID
	if (ownerRefID == 0x7)
	{
		g_lastRef = ref;
		return;
	}

	if (g_bShowNameOnlyCrime && !IsCrime(ref))
	{
		g_lastRef = ref;
		return;
	}

	g_lastRef = ref;

	//get original item name from global buffer
	const char* itemName = (const char*)0x11D9C48; //crosshairRefName
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

	Engine::Tile_SetString(tile, 0xFC4, modifiedName, true); //kTileValue_string
}

static char g_iniPath[MAX_PATH] = {0};

static void LoadINIPath()
{
	if (g_iniPath[0]) return;
	GetModuleFileNameA(nullptr, g_iniPath, MAX_PATH);
	char* lastSlash = strrchr(g_iniPath, '\\');
	if (lastSlash) *lastSlash = '\0';
	strcat_s(g_iniPath, "\\Data\\config\\itr-nvse.ini");
}

bool Init()
{
	LoadINIPath();
	g_bOwnerNameInfo = GetPrivateProfileIntA("Tweaks", "bOwnerNameInfo", 1, g_iniPath) != 0;
	g_bCompatMode = GetPrivateProfileIntA("OwnerNameInfo", "bCompatibilityMode", 1, g_iniPath) != 0;
	g_bShowFactionName = GetPrivateProfileIntA("OwnerNameInfo", "bShowFactionName", 1, g_iniPath) != 0;
	g_bShowNameOnlyCrime = GetPrivateProfileIntA("OwnerNameInfo", "bShowNameOnlyCrime", 1, g_iniPath) != 0;
	return true;
}

void UpdateSettings()
{
	LoadINIPath();
	g_bOwnerNameInfo = GetPrivateProfileIntA("Tweaks", "bOwnerNameInfo", 1, g_iniPath) != 0;
	g_bCompatMode = GetPrivateProfileIntA("OwnerNameInfo", "bCompatibilityMode", 1, g_iniPath) != 0;
	g_bShowFactionName = GetPrivateProfileIntA("OwnerNameInfo", "bShowFactionName", 1, g_iniPath) != 0;
	g_bShowNameOnlyCrime = GetPrivateProfileIntA("OwnerNameInfo", "bShowNameOnlyCrime", 1, g_iniPath) != 0;
}
}
