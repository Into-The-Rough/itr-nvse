#include "SaveFileSizeHandler.h"
#include "internal/SafeWrite.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/FormatLogic.h"
#include <cstdio>

static const UInt32 kTileValue_user1 = 0x1005;

struct BGSSaveLoadFileEntry
{
	char* name;
	UInt8 isInitialised;
	UInt8 isCorrupt;
	UInt8 gap06[2];
	UInt32 saveNumber;
	char* pcName;
	char* pcTitle;
	char* location;
	char* time;
};
static_assert(offsetof(BGSSaveLoadFileEntry, location) == 0x14);

typedef void (__thiscall *_ConstructSavegamePath)(void*, char*);
static const _ConstructSavegamePath ConstructSavegamePath = (_ConstructSavegamePath)0x84FF30;

namespace SaveFileSizeHandler
{
	static char g_savePath[MAX_PATH] = {0};
	static UInt32 g_chainTarget = 0;
	static Detours::CallDetour s_setupTileCall;

	static void EnsureSavePath()
	{
		if (g_savePath[0] == '\0' && *g_saveGameManagerPtr)
			ConstructSavegamePath(*g_saveGameManagerPtr, g_savePath);
	}

	static const UInt32 kAddr_HookSite = 0x7D6931;
	static const UInt32 kAddr_JnzPatch = 0x7D6806;

	void __cdecl OnSetupTile(void* tile, BGSSaveLoadFileEntry* entry)
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
		FormatLogic::FormatFileSize(fileSize, sizeStr, sizeof(sizeStr));

		static char newLoc[512];
		sprintf_s(newLoc, "%s - %s", entry->location, sizeStr);
		Engine::Tile_SetString(tile, kTileValue_user1, newLoc, true);
	}

	static void* g_savedTile = nullptr;
	static void* g_savedEntry = nullptr;

	void CallOnSetupTile()
	{
		OnSetupTile(g_savedTile, (BGSSaveLoadFileEntry*)g_savedEntry);
	}

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov g_savedTile, ecx           //ecx = tile (thiscall this), stash for the cdecl helper
			mov eax, [ebp+0xC]             //arg2 of caller's frame = BGSSaveLoadFileEntry*
			mov g_savedEntry, eax

			pushad
			pushfd
			call CallOnSetupTile           //reads the globals, fills size suffix into the tile
			popfd
			popad

			mov eax, g_chainTarget         //jump to the original callee for the overwritten call at kAddr_HookSite
			jmp eax
		}
	}

	static void InstallHooks()
	{
		if (!s_setupTileCall.WriteRelCall(kAddr_HookSite, Hook))
			return;
		g_chainTarget = s_setupTileCall.GetOverwrittenAddr();
		SafeWrite::WriteNop(kAddr_JnzPatch, 6);
	}

	bool Init()
	{
		InstallHooks();
		return true;
	}
}
