#include "UICommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"
#include "internal/CallTemplates.h"

namespace
{
	struct NiTArray_TileMenu {
		void* vtbl;
		void** data; //TileMenu*[]
		UInt16 capacity;
		UInt16 firstFreeEntry; //upper bound of live entries
		UInt16 numValidEntries;
		UInt16 growSize;
	};

	static NiTArray_TileMenu* g_TileMenuArray = (NiTArray_TileMenu*)0x11F3508;

	//Tile+0x20 = String name {char* m_data; UInt16 m_dataLen; UInt16 m_bufLen;}
	struct TileString { char* m_data; UInt16 m_dataLen; UInt16 m_bufLen; };
	//Tile+0x04 = DList<Tile> {Node* first; Node* last; UInt32 count;}
	struct TileNode { TileNode* next; TileNode* prev; void* data; }; //data = Tile*

	static const char* GetTileName(void* tile) { return ((TileString*)((UInt8*)tile + 0x20))->m_data; }
	static TileNode* GetFirstChild(void* tile) { return *(TileNode**)((UInt8*)tile + 0x04); }

	//vtable index 8 = GetShaderProperty
	static void* GetShaderProperty(void* tile)
	{
		auto vtbl = *(UInt32**)tile;
		typedef void* (__thiscall *fn)(void*);
		return ((fn)vtbl[8])(tile);
	}

	constexpr UInt32 kAddr_BSFixedString_Ctor = 0x438170;
	constexpr UInt32 kAddr_BSFixedString_Dtor = 0x4381D0;
	constexpr UInt32 kAddr_NiSourceTexture_Create = 0xA5FD70;
	constexpr UInt32 kAddr_NiSourceTexture_Params = 0x11A9598;

	static void* CreateNiSourceTexture(const char* path)
	{
		//engine expects full path from Data\ e.g. "Textures\Interface\HUD\foo.dds"
		char fullPath[520];
		_snprintf_s(fullPath, _TRUNCATE, "Textures\\%s", path);

		void* fixedStr = nullptr;
		ThisCall<void*>(kAddr_BSFixedString_Ctor, &fixedStr, fullPath);
		auto tex = CdeclCall<void*>(kAddr_NiSourceTexture_Create, &fixedStr, (void*)kAddr_NiSourceTexture_Params, true, false);
		CdeclCall(kAddr_BSFixedString_Dtor, &fixedStr);
		return tex;
	}

	static void* FindChild(void* parent, const char* name)
	{
		for (auto node = GetFirstChild(parent); node; node = node->next)
		{
			if (node->data && GetTileName(node->data) && !_stricmp(GetTileName(node->data), name))
				return node->data;
		}
		return nullptr;
	}

	static void* ResolveTilePath(const char* path)
	{
		char buf[512];
		strncpy_s(buf, path, _TRUNCATE);

		char* ctx = nullptr;
		char* menuName = strtok_s(buf, "\\/", &ctx);
		if (!menuName) return nullptr;

		void* tile = nullptr;
		for (UInt16 i = 0; i < g_TileMenuArray->firstFreeEntry; i++)
		{
			auto tm = g_TileMenuArray->data[i];
			if (tm && GetTileName(tm) && !_stricmp(GetTileName(tm), menuName))
			{
				tile = tm;
				break;
			}
		}
		if (!tile) return nullptr;

		char* segment;
		while ((segment = strtok_s(nullptr, "\\/", &ctx)) != nullptr)
		{
			tile = FindChild(tile, segment);
			if (!tile) return nullptr;
		}
		return tile;
	}

	static ParamInfo kParams_SetUIAlphaMap[2] = {
		{"tilePath",    kParamType_String, 0},
		{"texturePath", kParamType_String, 0},
	};

	DEFINE_COMMAND_PLUGIN(SetUIAlphaMap, "Applies an alpha mask texture to a UI tile", 0, 2, kParams_SetUIAlphaMap);

	bool Cmd_SetUIAlphaMap_Execute(COMMAND_ARGS)
	{
		*result = 0;

		char tilePath[512];
		char texturePath[512];
		if (!ExtractArgs(EXTRACT_ARGS, &tilePath, &texturePath))
			return true;

		auto tile = ResolveTilePath(tilePath);
		if (!tile)
		{
			if (IsConsoleMode())
				Console_Print("SetUIAlphaMap >> tile not found: %s", tilePath);
			return true;
		}

		auto shader = GetShaderProperty(tile);
		if (!shader)
		{
			if (IsConsoleMode())
				Console_Print("SetUIAlphaMap >> no shader property: %s", tilePath);
			return true;
		}

		auto tex = CreateNiSourceTexture(texturePath);
		if (!tex)
		{
			if (IsConsoleMode())
				Console_Print("SetUIAlphaMap >> failed to load: %s", texturePath);
			return true;
		}

		//TileShaderProperty::SetAlphaTexture
		ThisCall(0x7700B0, shader, tex);
		*result = 1;

		if (IsConsoleMode())
			Console_Print("SetUIAlphaMap >> applied %s to %s", texturePath, tilePath);

		return true;
	}
}

namespace UICommands {
bool Init(void* nvsePtr) { return true; }

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetUIAlphaMap);
}
}
