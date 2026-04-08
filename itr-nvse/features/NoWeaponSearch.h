#pragma once

#include "nvse/CommandTable.h"

class Actor;

namespace NoWeaponSearch {
	void Init();
	void RegisterCommands(void* nvse);
	void Set(Actor* actor, bool disable);
	bool Get(Actor* actor);
}

extern CommandInfo kCommandInfo_SetNoWeaponSearch;
extern CommandInfo kCommandInfo_GetNoWeaponSearch;
