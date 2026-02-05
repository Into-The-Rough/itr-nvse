#pragma once

bool OMFCH_Init(void* nvseInterface);
unsigned int OMFCH_GetOpcode();
void OMFCH_Update();
void OMFCH_ClearCallbacks();
