#pragma once

namespace CenterOnCellAltCommand
{
	void RegisterCommands(void* nvsePtr);
	void OnNewGame();
	void ClearPending();
	void Update();
}
