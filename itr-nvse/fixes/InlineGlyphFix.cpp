//scales inline SUActn button glyphs (&-sUActn...%a. / &...;) to the owning tile's zoom.
//vanilla renders button icons in an UNZOOMED geometry (1.0 transform) while surrounding text is zoom-transformed by the tile
//so at tile zoom != 100% the button appears oversized and offset far to the right of where the text ends
#include "InlineGlyphFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/globals.h"
#include "internal/settings.h"
#include <cstdio>

#define INLINE_GLYPH_FIX_DEBUG 0

namespace InlineGlyphFix
{
	constexpr UInt32 kAddr_Font_AddButton = 0xA14650;
	constexpr UInt32 kAddr_Font_ComputeButtonMetrics = 0xA14170;
	constexpr UInt32 kAddr_TileTextVtable_MakeNode = 0x1094880;
	constexpr UInt32 kAddr_Tile_GetFloat = 0xA011B0;

	constexpr UInt32 kTileValue_Zoom = 4024;
	constexpr UInt32 kTileValue_Font = 4025;

	constexpr UInt32 kFont_uiFontID = 0x08;
	constexpr UInt32 kFont_pData = 0x38;
	constexpr UInt32 kFontData_fFontSize = 0x00;

	struct ButtonIcon
	{
		float metric[4];
		float uv[4];
	};

	struct NiPoint3 { float x, y, z; };

	typedef void (__thiscall* Font_ComputeButtonMetrics_t)(void* font, ButtonIcon* icon, char* character);
	typedef void (__thiscall* Font_AddButton_t)(void* font, int iconIdx, void* triShape, NiPoint3* cursor);
	typedef void* (__thiscall* TileText_MakeNode_t)(void* tile);

	static Detours::JumpDetour s_metricsDetour;
	static Detours::JumpDetour s_addButtonDetour;
	static Font_ComputeButtonMetrics_t s_origComputeButtonMetrics = nullptr;
	static Font_AddButton_t s_origAddButton = nullptr;
	static TileText_MakeNode_t s_origMakeNode = nullptr;
	static bool s_installed = false;
	static bool g_enabled = false;

	//UI pipeline is single-threaded. every Font::AddButton call chain originates inside
	//TileText::MakeNode (sub_A21AF0, sole xref is vtable slot 0x1094880), so this is
	//always live when the font hooks run
	static void* s_currentTile = nullptr;

	void* __fastcall Hook_MakeNode(void* tile, void*)
	{
		void* prev = s_currentTile;
		s_currentTile = tile;
		void* result = s_origMakeNode(tile);
		s_currentTile = prev;
		return result;
	}

	static float GetFontSize(void* font)
	{
		if (!font) return 0.0f;

		void* fontData = *(void**)((char*)font + kFont_pData);
		if (!fontData) return 0.0f;

		return *(float*)((char*)fontData + kFontData_fFontSize);
	}

	static void* GetTileFont(void* tile, void* fallbackFont)
	{
		void** fontManager = ::GetFontManager();
		if (!tile || !fontManager) return fallbackFont;

		float fontValue = ThisCall<float>(kAddr_Tile_GetFloat, tile, kTileValue_Font);
		int fontID = (int)(fontValue + 0.5f);
		if (fontID >= 1 && fontID <= 8 && fontManager[fontID - 1])
			return fontManager[fontID - 1];

		return fallbackFont;
	}

	static float GetTileScale(void* tile, void* fallbackFont)
	{
		if (!tile)
			return 1.0f;

		void** fontManager = ::GetFontManager();
		if (!fontManager) return 1.0f;
		void* font1 = fontManager[0];
		if (!font1) return 1.0f;

		void* tileFont = GetTileFont(tile, fallbackFont);
		float font1Size = GetFontSize(font1);
		float tileFontSize = GetFontSize(tileFont);
		if (font1Size <= 0.0f || tileFontSize <= 0.0f) return 1.0f;

		float zoom = ThisCall<float>(kAddr_Tile_GetFloat, tile, kTileValue_Zoom);
		if (zoom <= 0.0f) zoom = 100.0f;

		return (tileFontSize / font1Size) * (zoom / 100.0f);
	}

	static float GetGlyphVisualScale()
	{
		int percent = Settings::iInlineGlyphVisualScalePercent;
		return percent > 0 ? (float)percent / 100.0f : 0.88f;
	}

	void __fastcall Hook_ComputeButtonMetrics(void* font, void*, ButtonIcon* icon, char* character)
	{
		s_origComputeButtonMetrics(font, icon, character);

		//hooks stay installed, behaviour gated by the runtime flag
		void* tile = s_currentTile;
		if (!g_enabled || !tile || !font || !icon) return;

		float scale = GetTileScale(tile, font);
		if (scale > 0.999f && scale < 1.001f) return;
		float visualScale = GetGlyphVisualScale();

		float oldSize = icon->metric[0];
		float oldAdvance = oldSize + icon->metric[3];
		float scaledSize = oldSize * scale;
		float newSize = scaledSize * visualScale;
		float newAdvance = oldAdvance * scale;

		icon->metric[0] = newSize;
		icon->metric[1] *= scale;
		icon->metric[3] = newAdvance - newSize;

#if INLINE_GLYPH_FIX_DEBUG
		FILE* fp = nullptr;
		fopen_s(&fp, "InlineGlyphFix.log", "a");
		if (fp) {
			float shrinkPerSide = (scaledSize - newSize) * 0.5f;
			void* tileFont = GetTileFont(tile, font);
			float zoom = ThisCall<float>(kAddr_Tile_GetFloat, tile, kTileValue_Zoom);
			int fontID = tileFont ? *(int*)((char*)tileFont + kFont_uiFontID) : 0;
			fprintf(fp, "Metrics: tileFont=%d scale=%.4f visual=%.2f size %.2f->%.2f advance %.2f->%.2f zoff=%.2f shrink=%.2f zoom=%.2f\n",
				fontID, scale, visualScale, oldSize, icon->metric[0], oldAdvance, newAdvance, icon->metric[2], shrinkPerSide, zoom);
			fclose(fp);
		}
#endif
	}

	void __fastcall Hook_AddButton(void* font, void*, int iconIdx, void* triShape, NiPoint3* cursor)
	{
		void* tile = s_currentTile;
		if (!g_enabled || !tile || !cursor)
		{
			s_origAddButton(font, iconIdx, triShape, cursor);
			return;
		}

		float scale = GetTileScale(tile, font);
		if (scale > 0.999f && scale < 1.001f)
		{
			s_origAddButton(font, iconIdx, triShape, cursor);
			return;
		}

		NiPoint3 adjusted = *cursor;
		float oldX = adjusted.x;
		adjusted.x *= scale;
		s_origAddButton(font, iconIdx, triShape, &adjusted);
		cursor->x = oldX + (adjusted.x - (oldX * scale));
	}

	static bool SwapVtableSlot(UInt32 slotAddr, UInt32 newFn, UInt32& outOrig)
	{
		UInt32* slot = (UInt32*)slotAddr;
		DWORD oldProtect;
		if (!VirtualProtect(slot, sizeof(UInt32), PAGE_EXECUTE_READWRITE, &oldProtect))
			return false;
		outOrig = *slot;
		*slot = newFn;
		VirtualProtect(slot, sizeof(UInt32), oldProtect, &oldProtect);
		return true;
	}

	static bool InstallHooks()
	{
		//prologue dump of Font::A14170: 55 8B EC 83 EC 08 89 4D FC (9 bytes)
		if (!s_metricsDetour.WriteRelJump(kAddr_Font_ComputeButtonMetrics, (UInt32)Hook_ComputeButtonMetrics, 9))
		{
			Log("InlineGlyphFix: failed to hook Font::A14170");
			return false;
		}
		s_origComputeButtonMetrics = s_metricsDetour.GetTrampoline<Font_ComputeButtonMetrics_t>();
		if (!s_origComputeButtonMetrics)
		{
			Log("InlineGlyphFix: A14170 trampoline null");
			s_metricsDetour.Remove();
			return false;
		}

		//prologue dump of Font::AddButton: 55 8B EC 81 EC D4 00 00 00 56 (10 bytes)
		if (!s_addButtonDetour.WriteRelJump(kAddr_Font_AddButton, (UInt32)Hook_AddButton, 10))
		{
			Log("InlineGlyphFix: failed to hook Font::AddButton");
			s_metricsDetour.Remove();
			s_origComputeButtonMetrics = nullptr;
			return false;
		}
		s_origAddButton = s_addButtonDetour.GetTrampoline<Font_AddButton_t>();
		if (!s_origAddButton)
		{
			Log("InlineGlyphFix: Font::AddButton trampoline null");
			s_addButtonDetour.Remove();
			s_metricsDetour.Remove();
			s_origComputeButtonMetrics = nullptr;
			return false;
		}

		//nonzero means a foreign vtable hook still chains through us, reswapping would capture it and cycle
		if (!s_origMakeNode)
		{
			UInt32 origMakeNode = 0;
			if (!SwapVtableSlot(kAddr_TileTextVtable_MakeNode, (UInt32)Hook_MakeNode, origMakeNode))
			{
				Log("InlineGlyphFix: failed to swap TileText vtable");
				s_addButtonDetour.Remove();
				s_metricsDetour.Remove();
				s_origComputeButtonMetrics = nullptr;
				s_origAddButton = nullptr;
				return false;
			}
			s_origMakeNode = (TileText_MakeNode_t)origMakeNode;
		}
		return true;
	}

	//hooks install once and stay resident, toggling only flips the runtime gate
	//so a foreign hook chained on top of ours never has to be unwound
	void SetEnabled(bool enabled)
	{
		if (enabled && !s_installed)
			s_installed = InstallHooks();
		g_enabled = enabled && s_installed;
	}

	void Init()
	{
		SetEnabled(true);
	}
}
