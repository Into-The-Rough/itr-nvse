#pragma once

bool FDH_Init(void* nvse);
void FDH_RegisterCommands(void* nvse);
UInt32 FDH_GetSetMultOpcode();
UInt32 FDH_GetGetMultOpcode();
