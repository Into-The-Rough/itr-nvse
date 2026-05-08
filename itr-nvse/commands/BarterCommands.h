#pragma once

namespace BarterCommands
{
	bool InitHooks();
	void ClearState();
	void RegisterCommands(void* nvsePtr);
}
