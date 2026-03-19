#include "SaveFileSizeHandler.h"
#include "internal/SafeWrite.h"
#include "internal/EngineFunctions.h"
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
static void** g_saveGameManager = (void**)0x11DE134;

namespace SaveFileSizeHandler
{
	static char g_savePath[MAX_PATH] = {0};
	static UInt32 g_chainTarget = 0;

	static void EnsureSavePath()
	{
		if (g_savePath[0] == '\0' && *g_saveGameManager)
			ConstructSavegamePath(*g_saveGameManager, g_savePath);
	}

	static void FormatFileSize(ULONGLONG bytes, char* out, size_t outSize)
	{
		if (bytes >= 1048576ULL)
			sprintf_s(out, outSize, "%.1f MB", bytes / 1048576.0);
		else if (bytes >= 1024ULL)
			sprintf_s(out, outSize, "%.1f KB", bytes / 1024.0);
		else
			sprintf_s(out, outSize, "%llu B", bytes);
	}

	static UInt32 ReadCallTarget(UInt32 addr)
	{
		UInt8* p = (UInt8*)addr;
		if (p[0] != 0xE8) return 0;
		return addr + 5 + *(SInt32*)(addr + 1);
	}

	static void SafeWriteBuf(UInt32 addr, const void* data, size_t len)
	{
		DWORD oldProtect;
		VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy((void*)addr, data, len);
		VirtualProtect((void*)addr, len, oldProtect, &oldProtect);
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
		FormatFileSize(fileSize, sizeStr, sizeof(sizeStr));

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
			mov g_savedTile, ecx
			mov eax, [ebp+0xC]
			mov g_savedEntry, eax

			pushad
			pushfd
			call CallOnSetupTile
			popfd
			popad

			mov eax, g_chainTarget
			jmp eax
		}
	}

	static void InstallHooks()
	{
		g_chainTarget = ReadCallTarget(kAddr_HookSite);
		if (g_chainTarget == 0)
			return;

		SafeWriteBuf(kAddr_JnzPatch, "\x90\x90\x90\x90\x90\x90", 6);
		SafeWrite::WriteRelCall(kAddr_HookSite, (UInt32)Hook);
	}

	bool Init()
	{
		InstallHooks();
		return true;
	}
}
