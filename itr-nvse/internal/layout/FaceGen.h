//FaceGen NPC data layout - eye form and eye model slots read by EyeMeshOverride
#pragma once

#include <cstddef>

#include "nvse/GameForms.h"

struct FaceGenNpcDataView {
	UInt8 pad00[0x8C];
	TESForm* eyeForm;
	UInt8 pad90[0xD8 - 0x90];
	void* leftEyeModel;
	void* rightEyeModel;
};

static_assert(offsetof(FaceGenNpcDataView, eyeForm) == 0x8C);
static_assert(offsetof(FaceGenNpcDataView, leftEyeModel) == 0xD8);
static_assert(offsetof(FaceGenNpcDataView, rightEyeModel) == 0xDC);

inline TESForm* FaceGenNpcDataGetEyeForm(void* npcData)
{
	return npcData ? reinterpret_cast<FaceGenNpcDataView*>(npcData)->eyeForm : nullptr;
}

inline void** FaceGenNpcDataGetLeftEyeModelSlot(void* npcData)
{
	return npcData ? &reinterpret_cast<FaceGenNpcDataView*>(npcData)->leftEyeModel : nullptr;
}

inline void** FaceGenNpcDataGetRightEyeModelSlot(void* npcData)
{
	return npcData ? &reinterpret_cast<FaceGenNpcDataView*>(npcData)->rightEyeModel : nullptr;
}
