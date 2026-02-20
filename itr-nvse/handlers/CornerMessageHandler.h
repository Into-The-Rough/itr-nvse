#pragma once

enum CornerMessageMetaType
{
    kCornerMeta_Generic = 0,
    kCornerMeta_ObjectiveDisplayed = 1,
    kCornerMeta_ObjectiveCompleted = 2,
    kCornerMeta_ReputationChange = 3,
};

bool CMH_Init(void* nvseInterface);
void CMH_TrackMessageMeta(const char* text, float displayTime, int metaType);
