#pragma once

bool OEPH_Init(void* nvseInterface);
unsigned int OEPH_GetOpcode();
void OEPH_BuildEntryMap();
void OEPH_ClearCallbacks();
