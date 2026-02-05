#pragma once

bool OMSCH_Init(void* nvseInterface);
unsigned int OMSCH_GetOpcode();
void OMSCH_Update();
void OMSCH_ClearCallbacks();
