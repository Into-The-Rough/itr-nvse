//TESChallenge layout - challenge form fields and threshold/completion data
#pragma once

#include <cstddef>

#include "nvse/GameForms.h"

struct TESChallengeDataView {
	UInt32 type;
	UInt32 threshold;
	UInt32 flags;
	UInt32 interval;
	UInt16 value1;
	UInt16 value2;
	UInt32 value3;
};

struct TESChallengeView {
	TESForm form;
	TESFullName fullName;
	TESDescription description;
	TESScriptableForm scriptable;
	TESIcon icon;
	BGSMessageIcon msgIcon;
	TESChallengeDataView data;
	UInt32 amount;
	UInt32 challengeFlags;
	TESForm* completionScript;
	TESForm* XNAM;
};

static_assert(sizeof(TESChallengeDataView) == 0x18);
static_assert(sizeof(TESChallengeView) == 0x7C);
static_assert(offsetof(TESChallengeView, fullName) == 0x18);
static_assert(offsetof(TESChallengeView, description) == 0x24);
static_assert(offsetof(TESChallengeView, icon) == 0x38);
static_assert(offsetof(TESChallengeView, data) == 0x54);
static_assert(offsetof(TESChallengeView, data.type) == 0x54);
static_assert(offsetof(TESChallengeView, data.threshold) == 0x58);
static_assert(offsetof(TESChallengeView, data.flags) == 0x5C);
static_assert(offsetof(TESChallengeView, data.interval) == 0x60);
static_assert(offsetof(TESChallengeView, data.value1) == 0x64);
static_assert(offsetof(TESChallengeView, amount) == 0x6C);
static_assert(offsetof(TESChallengeView, challengeFlags) == 0x70);
static_assert(offsetof(TESChallengeView, completionScript) == 0x74);

inline TESChallengeView* TESChallengeAsView(TESForm* form)
{
	return reinterpret_cast<TESChallengeView*>(form);
}
