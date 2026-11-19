#pragma once

#include "internal/MenuLayout.h"

namespace MessageBoxQuickClose {
	using MessageMenuHandleClickObserver = void (*)(MessageMenuView* menu, SInt32 tileID, TileView* clickedTile);

	bool Init();
	bool IsInstalled();
	void SetHandleClickObserver(MessageMenuHandleClickObserver observer);
}
