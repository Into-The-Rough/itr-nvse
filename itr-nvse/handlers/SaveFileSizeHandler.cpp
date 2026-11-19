#include "SaveFileSizeHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"
#include <cstdio>

static constexpr UInt32 kTileValue_user1 = 0x1005;

using ConstructSavegamePath_t = void(__thiscall*)(void*, char*);
static const ConstructSavegamePath_t ConstructSavegamePath = (ConstructSavegamePath_t)0x84FF30;

namespace SaveFileSizeHandler {
	static char g_savePath[MAX_PATH] = {0};
	static UInt32 g_chainTarget = 0;
	static Detours::CallDetour s_setupTileCall;

	static void EnsureSavePath()
	{
		if (g_savePath[0] == '\0' && *g_saveGameManagerPtr)
			ConstructSavegamePath(*g_saveGameManagerPtr, g_savePath);
	}

	//0x7D6931 sets _initialized at end of StartMenu::InitializeSaveLoadListItem (runs once per tile)
	static constexpr UInt32 kAddr_HookSite = 0x7D6931;

	void __cdecl OnSetupTile(void* tile, BGSSaveLoadFileEntryView* entry)
	{
		if (!entry || !entry->name || !entry->location)
			return;

		EnsureSavePath();
		if (g_savePath[0] == '\0')
			return;

		char fullPath[MAX_PATH];
		sprintf_s(fullPath, "%s%s.fos", g_savePath, entry->name);

		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (!GetFileAttributesExA(fullPath, GetFileExInfoStandard, &fad))
			return;

		ULONGLONG fileSize = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
		char sizeStr[32];
		if (fileSize >= 1048576ULL)
			sprintf_s(sizeStr, "%.1f MB", fileSize / 1048576.0);
		else if (fileSize >= 1024ULL)
			sprintf_s(sizeStr, "%.1f KB", fileSize / 1024.0);
		else
			sprintf_s(sizeStr, "%llu B", fileSize);

		static char newLoc[512];
		_snprintf_s(newLoc, sizeof(newLoc), _TRUNCATE, "%s - %s", entry->location, sizeStr);
		Engine::Tile_SetString(tile, kTileValue_user1, newLoc, true);
	}

	static void* g_savedTile = nullptr;
	static void* g_savedEntry = nullptr;

	void CallOnSetupTile()
	{
		OnSetupTile(g_savedTile, (BGSSaveLoadFileEntryView*)g_savedEntry);
	}

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov g_savedTile, ecx //ecx = tile (thiscall this), stash for the cdecl helper
			mov eax, [ebp+0xC] //arg2 of caller's frame = BGSSaveLoadFileEntry*
			mov g_savedEntry, eax

			pushad
			pushfd
			call CallOnSetupTile //reads the globals, fills size suffix into the tile
			popfd
			popad

			mov eax, g_chainTarget //jump to the original callee for the overwritten call at kAddr_HookSite
			jmp eax
		}
	}

	static void InstallHooks()
	{
		if (!s_setupTileCall.WriteRelCall(kAddr_HookSite, Hook))
			return;
		g_chainTarget = s_setupTileCall.GetOverwrittenAddr();
	}

	bool Init(void* nvseInterface)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
		if (nvse->isEditor) return false;

		InstallHooks();
		return true;
	}
}
