#pragma once

enum CornerMessageMetaType
{
    kCornerMeta_Generic = 0,
    kCornerMeta_ObjectiveDisplayed = 1,
    kCornerMeta_ObjectiveCompleted = 2,
    kCornerMeta_ReputationChange = 3,
};

namespace CornerMessageHandler {
	bool Init(void* nvseInterface);
	void TrackMessageMeta(const char* text, float displayTime, int metaType);
}
