//NOT hot-reloadable - requires game restart

#include "nvse/PluginAPI.h"
#include "nvse/GameUI.h"
#include <Windows.h>

#include "MessageBoxQuickClose.h"

namespace MessageBoxQuickClose
{
	constexpr UInt32 kOffset_HandleSpecialKeyInput = 0x38;
	constexpr UInt32 kOffset_HandleClick = 0x0C;

	typedef bool (__thiscall* _MessageMenu_HandleSpecialKeyInput)(MessageMenu* menu, int code, float keyState);
	typedef void (__thiscall* _MessageMenu_HandleClick)(MessageMenu* menu, SInt32 tileID, Tile* clickedTile);

	_MessageMenu_HandleSpecialKeyInput OriginalHandleSpecialKeyInput = nullptr;
	_MessageMenu_HandleClick OriginalHandleClick = nullptr;
}

static void __stdcall SafeWrite32(UInt32 addr, UInt32 data)
{
	DWORD oldProtect;
	VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProtect);
	*reinterpret_cast<UInt32*>(addr) = data;
	VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProtect, &oldProtect);
}

static Tile::Value* GetTileValue(const Tile* tile, UInt32 typeID)
{
	if (!tile) return nullptr;

	const auto& values = tile->values;
	Tile::Value** data = values.Data();
	UInt32 count = values.Size();

	UInt32 left = 0;
	UInt32 right = count;

	while (left < right)
	{
		UInt32 mid = (left + right) / 2;
		Tile::Value* value = data[mid];

		if (value->id < typeID)
			left = mid + 1;
		else if (value->id == typeID)
			return value;
		else
			right = mid;
	}

	return nullptr;
}

static float GetTileValueFloat(const Tile* tile, UInt32 id)
{
	Tile::Value* value = GetTileValue(tile, id);
	return value ? value->num : 0.0f;
}

bool __fastcall MessageMenu_HandleSpecialKeyInput_Hook(MessageMenu* menu, void* edx, int code, float keyState)
{
	using namespace MessageBoxQuickClose;

	if (!menu)
		return OriginalHandleSpecialKeyInput ? OriginalHandleSpecialKeyInput(menu, code, keyState) : false;

	if ((code == kEnter || code == kAltEnter || code == kSpace) && keyState > 0.0f)
	{
		auto* head = menu->buttonList.list.Head();
		if (head && head->data && head->data->tile)
		{
			Tile* buttonTile = head->data->tile;
			UInt32 tileID = static_cast<UInt32>(GetTileValueFloat(buttonTile, kTileValue_id));
			menu->HandleClick(tileID, buttonTile);
			return true;
		}
	}

	return OriginalHandleSpecialKeyInput ? OriginalHandleSpecialKeyInput(menu, code, keyState) : false;
}

//fix controller always clicking first button instead of selected button
void __fastcall MessageMenu_HandleClick_Hook(MessageMenu* menu, void* edx, SInt32 tileID, Tile* clickedTile)
{
	using namespace MessageBoxQuickClose;

	if (!menu)
	{
		if (OriginalHandleClick)
			OriginalHandleClick(menu, tileID, clickedTile);
		return;
	}

	Tile* selectedTile = menu->buttonList.selected;
	if (selectedTile && selectedTile != clickedTile)
	{
		UInt32 selectedID = static_cast<UInt32>(GetTileValueFloat(selectedTile, kTileValue_id));
		if (OriginalHandleClick)
			OriginalHandleClick(menu, selectedID, selectedTile);
		return;
	}

	if (OriginalHandleClick)
		OriginalHandleClick(menu, tileID, clickedTile);
}

bool MBQC_Init()
{
	using namespace MessageBoxQuickClose;

	UInt32* vtbl = reinterpret_cast<UInt32*>(kVtbl_MessageMenu);

	OriginalHandleSpecialKeyInput = reinterpret_cast<_MessageMenu_HandleSpecialKeyInput>(vtbl[kOffset_HandleSpecialKeyInput / 4]);
	OriginalHandleClick = reinterpret_cast<_MessageMenu_HandleClick>(vtbl[kOffset_HandleClick / 4]);

	SafeWrite32(kVtbl_MessageMenu + kOffset_HandleSpecialKeyInput, reinterpret_cast<UInt32>(MessageMenu_HandleSpecialKeyInput_Hook));
	SafeWrite32(kVtbl_MessageMenu + kOffset_HandleClick, reinterpret_cast<UInt32>(MessageMenu_HandleClick_Hook));

	return true;
}
