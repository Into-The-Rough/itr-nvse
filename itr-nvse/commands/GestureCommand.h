#pragma once

#include "nvse/CommandTable.h"

namespace GestureCommand {
	void Init();
	void Update();
	void Reset();
	void RegisterCommands(void* nvse);
}

extern CommandInfo kCommandInfo_Gesture;
