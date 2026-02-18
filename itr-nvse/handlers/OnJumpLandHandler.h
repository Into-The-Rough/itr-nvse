#pragma once

bool OJLH_Init(void* nvseInterface);
unsigned int OJLH_GetLandedOpcode();
unsigned int OJLH_GetJumpOpcode();
void OJLH_Update();
void OJLH_ClearCallbacks();
