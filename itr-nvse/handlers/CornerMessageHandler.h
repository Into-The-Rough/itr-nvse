#pragma once

enum CornerMessageMetaType
{
    kCornerMeta_Generic = 0,
    kCornerMeta_ObjectiveDisplayed = 1,
    kCornerMeta_ObjectiveCompleted = 2,
    kCornerMeta_ReputationChange = 3,
};

bool CMH_Init(void* nvseInterface);
unsigned int CMH_GetOpcode();
void CMH_TrackMessageMeta(const char* text, float displayTime, int metaType);
void CMH_ClearCallbacks();
