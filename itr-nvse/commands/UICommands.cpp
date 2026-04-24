#include "UICommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"
#include "internal/CallTemplates.h"

#include <cmath>
#include <cstring>

namespace
{
	struct NiTArray_TileMenu
	{
		void* vtbl;
		void** data;
		UInt16 capacity;
		UInt16 firstFreeEntry;
		UInt16 numValidEntries;
		UInt16 growSize;
	};

	struct TileString
	{
		char* m_data;
		UInt16 m_dataLen;
		UInt16 m_bufLen;
	};

	struct TileNode
	{
		TileNode* next;
		TileNode* prev;
		void* data;
	};

	static NiTArray_TileMenu* g_TileMenuArray = reinterpret_cast<NiTArray_TileMenu*>(0x11F3508);

	constexpr UInt32 kTileType_Image = 0x386;
	constexpr UInt32 kTileType_Radial = 0x38C;

	constexpr UInt32 kAddr_BSFixedString_Ctor = 0x438170;
	constexpr UInt32 kAddr_BSFixedString_Dtor = 0x4381D0;
	constexpr UInt32 kAddr_NiSourceTexture_Create = 0xA5FD70;
	constexpr UInt32 kAddr_TileShaderProperty_SetAlphaTexture = 0x7700B0;
	constexpr UInt32 kAddr_TileShaderProperty_SetRotates = 0x7700D0;
	constexpr UInt32 kAddr_TileShaderProperty_SetTexOffset = 0x77A450;
	constexpr UInt32 kAddr_BSShaderProperty_ClearRenderPasses = 0xBAA000;

	constexpr UInt32 kTileShaderProperty_Rotates = 0x91;

	static const char* GetTileName(void* tile)
	{
		return tile ? reinterpret_cast<TileString*>(reinterpret_cast<UInt8*>(tile) + 0x20)->m_data : nullptr;
	}

	static TileNode* GetFirstChild(void* tile)
	{
		return tile ? *reinterpret_cast<TileNode**>(reinterpret_cast<UInt8*>(tile) + 0x04) : nullptr;
	}

	static UInt32 GetTileType(void* tile)
	{
		auto vtbl = *reinterpret_cast<UInt32**>(tile);
		typedef UInt32(__thiscall* GetTypeFn)(void*);
		return reinterpret_cast<GetTypeFn>(vtbl[3])(tile);
	}

	static bool IsImageTile(void* tile)
	{
		if (!tile)
			return false;

		const UInt32 type = GetTileType(tile);
		return type == kTileType_Image || type == kTileType_Radial;
	}

	static void* GetShaderProperty(void* tile)
	{
		if (!tile)
			return nullptr;

		auto vtbl = *reinterpret_cast<UInt32**>(tile);
		typedef void*(__thiscall* GetShaderPropertyFn)(void*);
		return reinterpret_cast<GetShaderPropertyFn>(vtbl[8])(tile);
	}

	static bool ShaderRotates(void* shader)
	{
		return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(shader) + kTileShaderProperty_Rotates) != 0;
	}

	static void InvalidateRenderPasses(void* shader)
	{
		ThisCall<void>(kAddr_BSShaderProperty_ClearRenderPasses, shader);
	}

	static void* FindChild(void* parent, const char* name)
	{
		for (auto node = GetFirstChild(parent); node; node = node->next)
		{
			const char* childName = GetTileName(node->data);
			if (childName && !_stricmp(childName, name))
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
		if (!menuName)
			return nullptr;

		void* tile = nullptr;
		for (UInt16 i = 0; i < g_TileMenuArray->firstFreeEntry; i++)
		{
			auto tm = g_TileMenuArray->data[i];
			const char* tileName = GetTileName(tm);
			if (tileName && !_stricmp(tileName, menuName))
			{
				tile = tm;
				break;
			}
		}

		if (!tile)
			return nullptr;

		char* segment = nullptr;
		while ((segment = strtok_s(nullptr, "\\/", &ctx)) != nullptr)
		{
			tile = FindChild(tile, segment);
			if (!tile)
				return nullptr;
		}

		return tile;
	}

	static bool PathStartsWithTextures(const char* path)
	{
		return path && (!_strnicmp(path, "textures\\", 9) || !_strnicmp(path, "textures/", 9));
	}

	static void* CreateNiSourceTexture(const char* path)
	{
		if (!path || !path[0])
			return nullptr;

		char fullPath[520];
		if (PathStartsWithTextures(path))
			strncpy_s(fullPath, path, _TRUNCATE);
		else
			_snprintf_s(fullPath, _TRUNCATE, "Textures\\%s", path);

		void* fixedString = nullptr;
		ThisCall<void*>(kAddr_BSFixedString_Ctor, &fixedString, fullPath);
		void* texture = CdeclCall<void*>(kAddr_NiSourceTexture_Create, &fixedString, reinterpret_cast<void*>(0x11A9598), true, false);
		CdeclCall(kAddr_BSFixedString_Dtor, &fixedString);
		return texture;
	}

	static float WrapUnit(float value)
	{
		if (!std::isfinite(value))
			return 0.0f;

		value -= std::floor(value);
		return value < 0.0f ? value + 1.0f : value;
	}

	static ParamInfo kParams_SetUIAlphaMap[2] = {
		{"tilePath", kParamType_String, 0},
		{"texturePath", kParamType_String, 0},
	};

	static ParamInfo kParams_SetUITexOffset[3] = {
		{"tilePath", kParamType_String, 0},
		{"xOffset", kParamType_Float, 0},
		{"yOffset", kParamType_Float, 0},
	};

	DEFINE_COMMAND_PLUGIN(SetUIAlphaMap, "Applies an alpha mask texture to a UI image tile", 0, 2, kParams_SetUIAlphaMap);
	DEFINE_COMMAND_PLUGIN(SetUITexOffset, "Sets a UI image tile texture scroll offset", 0, 3, kParams_SetUITexOffset);

	bool Cmd_SetUIAlphaMap_Execute(COMMAND_ARGS)
	{
		*result = 0;

		char tilePath[512];
		char texturePath[512];
		if (!ExtractArgs(EXTRACT_ARGS, &tilePath, &texturePath))
			return true;

		void* tile = ResolveTilePath(tilePath);
		if (!IsImageTile(tile))
		{
			if (IsConsoleMode())
				Console_Print("SetUIAlphaMap >> image tile not found: %s", tilePath);
			return true;
		}

		void* shader = GetShaderProperty(tile);
		if (!shader)
		{
			if (IsConsoleMode())
				Console_Print("SetUIAlphaMap >> tile has no shader property: %s", tilePath);
			return true;
		}

		void* texture = CreateNiSourceTexture(texturePath);
		if (!texture)
		{
			if (IsConsoleMode())
				Console_Print("SetUIAlphaMap >> texture not loaded: %s", texturePath);
			return true;
		}

		ThisCall<void>(kAddr_TileShaderProperty_SetAlphaTexture, shader, texture);
		InvalidateRenderPasses(shader);
		*result = 1;
		return true;
	}

	bool Cmd_SetUITexOffset_Execute(COMMAND_ARGS)
	{
		*result = 0;

		char tilePath[512];
		float xOffset = 0.0f;
		float yOffset = 0.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &tilePath, &xOffset, &yOffset))
			return true;

		void* tile = ResolveTilePath(tilePath);
		if (!IsImageTile(tile))
		{
			if (IsConsoleMode())
				Console_Print("SetUITexOffset >> image tile not found: %s", tilePath);
			return true;
		}

		void* shader = GetShaderProperty(tile);
		if (!shader)
		{
			if (IsConsoleMode())
				Console_Print("SetUITexOffset >> tile has no shader property: %s", tilePath);
			return true;
		}

		const bool wasScrolling = ShaderRotates(shader);
		ThisCall<void>(kAddr_TileShaderProperty_SetRotates, shader, true);
		ThisCall<void>(kAddr_TileShaderProperty_SetTexOffset, shader, WrapUnit(xOffset), WrapUnit(yOffset));
		if (!wasScrolling)
			InvalidateRenderPasses(shader);
		*result = 1;
		return true;
	}
}

namespace UICommands
{
	bool Init(void* nvsePtr)
	{
		return true;
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_SetUIAlphaMap);
	}

	void RegisterCommands2(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_SetUITexOffset);
	}
}
