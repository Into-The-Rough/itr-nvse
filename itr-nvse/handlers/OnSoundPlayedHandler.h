#pragma once

bool OSPH_Init(void* nvseInterface);
unsigned int OSPH_GetOpcode();
unsigned int OSPH_GetCompletionOpcode();
void OSPH_Update();
void OSPH_ClearCallbacks();
