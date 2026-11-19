//requires restart after hook install

#include "MessageBoxQuickClose.h"
#include "internal/EngineFunctions.h"
#include "internal/SafeWrite.h"

namespace MessageBoxQuickClose {
	constexpr UInt32 kVtbl_MessageMenu = 0x107566C;
	constexpr UInt32 kOffset_HandleSpecialKeyInput = 0x38;
	constexpr UInt32 kOffset_HandleClick = 0x0C;

	using MessageMenu_HandleSpecialKeyInput_t = bool(__thiscall*)(MessageMenuView* menu, int code, float keyState);
	using MessageMenu_HandleClick_t = void(__thiscall*)(MessageMenuView* menu, SInt32 tileID, TileView* clickedTile);

	MessageMenu_HandleSpecialKeyInput_t OriginalHandleSpecialKeyInput = nullptr;
	MessageMenu_HandleClick_t OriginalHandleClick = nullptr;
	MessageMenuHandleClickObserver HandleClickObserver = nullptr;
	bool HooksInstalled = false;
}

static float GetTileValueFloat(const TileView* tile, UInt32 id)
{
	TileValueView* value = TileGetValue(tile, id);
	return value ? value->num : 0.0f;
}

static void DispatchHandleClick(MessageMenuView* menu, SInt32 tileID, TileView* clickedTile)
{
	using namespace MessageBoxQuickClose;

	auto* vtbl = *reinterpret_cast<UInt32**>(menu);
	auto handleClick = reinterpret_cast<MessageMenu_HandleClick_t>(vtbl[kOffset_HandleClick / 4]);
	handleClick(menu, tileID, clickedTile);
}

static void NotifyHandleClickObserver(MessageMenuView* menu, SInt32 tileID, TileView* clickedTile)
{
	using namespace MessageBoxQuickClose;

	if (HandleClickObserver)
		HandleClickObserver(menu, tileID, clickedTile);
}

bool __fastcall Hook_MessageMenu_HandleSpecialKeyInput(MessageMenuView* menu, void* edx, int code, float keyState)
{
	using namespace MessageBoxQuickClose;

	if (!menu)
		return OriginalHandleSpecialKeyInput(menu, code, keyState);

	if ((code == kMenuKey_Enter || code == kMenuKey_AltEnter || code == kMenuKey_Space) && keyState > 0.0f) {
		auto* head = menu->buttonList.list.Head();
		if (head && head->data && head->data->tile) {
			TileView* buttonTile = head->data->tile;
			UInt32 tileID = static_cast<UInt32>(GetTileValueFloat(buttonTile, kTileTrait_id));
			DispatchHandleClick(menu, tileID, buttonTile);
			return true;
		}
	}

	return OriginalHandleSpecialKeyInput(menu, code, keyState);
}

void __fastcall Hook_MessageMenu_HandleClick(MessageMenuView* menu, void* edx, SInt32 tileID, TileView* clickedTile)
{
	using namespace MessageBoxQuickClose;

	if (!menu) {
		OriginalHandleClick(menu, tileID, clickedTile);
		return;
	}

	TileView* selectedTile = menu->buttonList.selected;
	if (selectedTile && selectedTile != clickedTile) {
		UInt32 selectedID = static_cast<UInt32>(GetTileValueFloat(selectedTile, kTileTrait_id));
		OriginalHandleClick(menu, selectedID, selectedTile);
		NotifyHandleClickObserver(menu, selectedID, selectedTile);
		return;
	}

	OriginalHandleClick(menu, tileID, clickedTile);
	NotifyHandleClickObserver(menu, tileID, clickedTile);
}

namespace MessageBoxQuickClose {
	bool Init()
	{
		if (HooksInstalled)
			return true;

		UInt32* vtbl = reinterpret_cast<UInt32*>(kVtbl_MessageMenu);

		OriginalHandleSpecialKeyInput = reinterpret_cast<MessageMenu_HandleSpecialKeyInput_t>(vtbl[kOffset_HandleSpecialKeyInput / 4]);
		OriginalHandleClick = reinterpret_cast<MessageMenu_HandleClick_t>(vtbl[kOffset_HandleClick / 4]);

		SafeWrite::Write32(kVtbl_MessageMenu + kOffset_HandleSpecialKeyInput, reinterpret_cast<UInt32>(Hook_MessageMenu_HandleSpecialKeyInput));
		SafeWrite::Write32(kVtbl_MessageMenu + kOffset_HandleClick, reinterpret_cast<UInt32>(Hook_MessageMenu_HandleClick));
		HooksInstalled = true;

		return true;
	}

	bool IsInstalled()
	{
		return HooksInstalled;
	}

	void SetHandleClickObserver(MessageMenuHandleClickObserver observer)
	{
		HandleClickObserver = observer;
	}
}
