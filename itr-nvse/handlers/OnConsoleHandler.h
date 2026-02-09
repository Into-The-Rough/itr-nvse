#pragma once

bool OCH_Init(void* nvseInterface);
unsigned int OCH_GetOpenOpcode();
unsigned int OCH_GetCloseOpcode();
void OCH_ClearCallbacks();
