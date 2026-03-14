#pragma once

struct Mat3;

bool DCH_Init(void* nvse);
void DCH_Update();
bool DCH_InstallCameraHooks();
void DCH_SetExternalRotation(const Mat3& rot);
void DCH_ClearExternalRotation();
