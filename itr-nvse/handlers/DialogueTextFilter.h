#pragma once

struct NVSEInterface_DTF;
struct Script_DTF;

bool DTF_Init(void* nvseInterface);
bool DTF_AddFilter(const char* filterText, void* callback);
bool DTF_RemoveFilter(const char* filterText, void* callback);
unsigned int DTF_GetOpcode();
