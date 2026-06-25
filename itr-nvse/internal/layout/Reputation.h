//TESReputation layout - reputation form fields read by ELMO
#pragma once

#include <cstddef>

#include "nvse/GameForms.h"

struct TESReputationView {
	TESForm form;
	TESFullName fullName;
	TESIcon icon;
	BGSMessageIcon msgIcon;
	float maxReputation;
	float positiveReputation;
	float negativeReputation;
	UInt32 reputationChangedWasPositive;
};

static_assert(sizeof(TESReputationView) == 0x50);
static_assert(offsetof(TESReputationView, fullName) == 0x18);
static_assert(offsetof(TESReputationView, icon) == 0x24);
static_assert(offsetof(TESReputationView, msgIcon) == 0x30);
static_assert(offsetof(TESReputationView, maxReputation) == 0x40);
static_assert(offsetof(TESReputationView, positiveReputation) == 0x44);
static_assert(offsetof(TESReputationView, negativeReputation) == 0x48);
static_assert(offsetof(TESReputationView, reputationChangedWasPositive) == 0x4C);

inline TESFullName* TESReputationGetFullName(void* reputation)
{
	return reputation ? &reinterpret_cast<TESReputationView*>(reputation)->fullName : nullptr;
}

inline bool TESReputationLastChangeWasPositive(void* reputation)
{
	return reputation && reinterpret_cast<TESReputationView*>(reputation)->reputationChangedWasPositive == 1;
}
