#pragma once

bool OKSH_Init(void* nvseInterface);
unsigned int OKSH_GetDisabledOpcode();
unsigned int OKSH_GetEnabledOpcode();
void OKSH_ClearCallbacks();
