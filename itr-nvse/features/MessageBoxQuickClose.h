#pragma once

#include "internal/UIMinimal.h"

namespace MessageBoxQuickClose {
	using MessageMenuHandleClickObserver = void (*)(UIMinimal::MessageMenu* menu, SInt32 tileID, UIMinimal::Tile* clickedTile);

	bool Init();
	bool IsInstalled();
	void SetHandleClickObserver(MessageMenuHandleClickObserver observer);
}
