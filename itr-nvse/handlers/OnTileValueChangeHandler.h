#pragma once

namespace OnTileValueChangeHandler {
	bool Init(void* nvseInterface);
	void RegisterCommands(void* nvse);
	void ClearState();
}
