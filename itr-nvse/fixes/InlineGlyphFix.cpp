//scales inline SUActn button glyphs (&-sUActn...%a. / &...;) to the owning
//tile's zoom. vanilla renders button icons in an UNZOOMED geometry (1.0
//transform) while surrounding text is zoom-transformed by the tile, so at
//tile zoom != 100% the button appears oversized and offset far to the
//right of where the text ends (ELMO, B42 Notify, Smooth Notifications).
//
//strategy: scale all four ButtonIcon metric fields by the tile scale so
//the button appears proportional to the zoomed text; apply an extra
//visualScale shrink to metric[0] with a matching metric[2] re-center;
//Hook_AddButton pre-scales cursor.x so the button's unzoomed left edge
//(cursor.x*scale + metric[1]*scale) sits next to where zoomed text ends.

#include "InlineGlyphFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
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
	constexpr UInt32 kAddr_FontManager_pSingleton = 0x11F33F8;

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

	static void* s_currentTile = nullptr; //UI pipeline is single-threaded
	static void* s_lastTile = nullptr;

	void* __fastcall Hook_MakeNode(void* tile, void*)
	{
		void* prev = s_currentTile;
		s_currentTile = tile;
		s_lastTile = tile;
		void* result = s_origMakeNode(tile);
		s_currentTile = prev;
		return result;
	}

	static void** GetFontManager()
	{
		return *(void***)kAddr_FontManager_pSingleton;
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
		void** fontManager = GetFontManager();
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

		void** fontManager = GetFontManager();
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

	static void* GetActiveTile()
	{
		return s_currentTile ? s_currentTile : s_lastTile;
	}

	static float GetGlyphVisualScale()
	{
		int percent = Settings::iInlineGlyphVisualScalePercent;
		return percent > 0 ? (float)percent / 100.0f : 0.88f;
	}

	void __fastcall Hook_ComputeButtonMetrics(void* font, void*, ButtonIcon* icon, char* character)
	{
		s_origComputeButtonMetrics(font, icon, character);

		void* tile = s_currentTile;
		if (!tile || !font || !icon) return;
		s_lastTile = tile;

		float scale = GetTileScale(tile, font);
		if (scale > 0.999f && scale < 1.001f) return;
		float visualScale = GetGlyphVisualScale();

		//metric[0]=size (vanilla coord), [1]=X bearing (vanilla), [2]=Z offset
		//(ALREADY in tile font coord: vanilla sets it to this->fLargestHeight),
		//[3]=post-kern (vanilla coord). top = cursor.z + metric[2], bottom =
		//top - size. shrinking size without touching metric[2] biases the
		//glyph upward, so shift metric[2] down by the per-side shrink amount
		//to keep the center fixed.
		//all four metric fields are in the button's UNZOOMED coord space (the
		//button icon geometry renders at 1.0 regardless of the tile's zoom).
		//scale each by the tile factor so the button appears proportional to
		//the zoomed text, then recenter vertically for the visualScale shrink.
		float oldSize = icon->metric[0];
		float oldAdvance = oldSize + icon->metric[3];
		float scaledSize = oldSize * scale;
		float newSize = scaledSize * visualScale;
		float newAdvance = oldAdvance * scale;
		float shrinkPerSide = (scaledSize - newSize) * 0.5f;

		icon->metric[0] = newSize;
		icon->metric[1] *= scale;
		icon->metric[3] = newAdvance - newSize;

#if INLINE_GLYPH_FIX_DEBUG
		FILE* fp = nullptr;
		fopen_s(&fp, "InlineGlyphFix.log", "a");
		if (fp) {
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
		if (!s_origAddButton)
			return;

		void* tile = GetActiveTile();
		if (!tile || !cursor)
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
		return true;
	}

	static void UninstallHooks()
	{
		if (s_origMakeNode)
		{
			UInt32 dummy = 0;
			SwapVtableSlot(kAddr_TileTextVtable_MakeNode, (UInt32)s_origMakeNode, dummy);
			s_origMakeNode = nullptr;
		}
		s_addButtonDetour.Remove();
		s_metricsDetour.Remove();
		s_origComputeButtonMetrics = nullptr;
		s_origAddButton = nullptr;
		s_currentTile = nullptr;
		s_lastTile = nullptr;
	}

	void SetEnabled(bool enabled)
	{
		if (enabled == s_installed) return;
		if (enabled)
			s_installed = InstallHooks();
		else
		{
			UninstallHooks();
			s_installed = false;
		}
	}

	void Init()
	{
		SetEnabled(true);
	}
}
