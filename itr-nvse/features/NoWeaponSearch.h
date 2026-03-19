#pragma once

#include "nvse/CommandTable.h"

namespace NoWeaponSearch {
	void Init();
	void RegisterCommands(void* nvse);
}

extern CommandInfo kCommandInfo_SetNoWeaponSearch;
extern CommandInfo kCommandInfo_GetNoWeaponSearch;
