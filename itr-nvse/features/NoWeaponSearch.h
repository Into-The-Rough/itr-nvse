#pragma once

#include "nvse/CommandTable.h"

void NoWeaponSearch_Init();
void NoWeaponSearch_RegisterCommands(void* nvse);

extern CommandInfo kCommandInfo_SetNoWeaponSearch;
extern CommandInfo kCommandInfo_GetNoWeaponSearch;
