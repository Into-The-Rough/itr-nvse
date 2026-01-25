#pragma once
//fixed-size cooldown tracker - extracted from LocationVisitPopup
//tracks items that shouldn't trigger again until cooldown expires and user has "left"

#include <cstdint>

template<int MaxSize>
class CooldownTracker {
public:
	uint32_t ids[MaxSize] = {0};
	uint32_t lastSeen[MaxSize] = {0};
	uint32_t popupTime[MaxSize] = {0};
	bool hasLeft[MaxSize] = {false};
	int count = 0;

	int Capacity() const { return MaxSize; }
	int Count() const { return count; }

	//update all entries - mark as "left" if not seen for leaveThresholdMs
	void UpdateCooldowns(uint32_t now, uint32_t leaveThresholdMs)
	{
		for (int i = 0; i < count; i++)
			if (!hasLeft[i] && now - lastSeen[i] > leaveThresholdMs)
				hasLeft[i] = true;
	}

	//check if id should trigger. returns:
	// 0 = should trigger (left and cooldown expired)
	// 1 = don't trigger (still present, or in cooldown)
	int Check(uint32_t id, uint32_t now, uint32_t cooldownMs)
	{
		for (int i = 0; i < count; i++) {
			if (ids[i] == id) {
				lastSeen[i] = now;
				if (!hasLeft[i])
					return 1;
				if (popupTime[i] && now - popupTime[i] < cooldownMs)
					return 1;
				return 0;
			}
		}
		//new entry
		if (count < MaxSize) {
			ids[count] = id;
			popupTime[count] = 0;
			hasLeft[count] = false;
			lastSeen[count] = now;
			count++;
		}
		return 1; //first visit, don't trigger
	}

	//mark that popup was shown for this id
	void MarkShown(uint32_t id, uint32_t now)
	{
		for (int i = 0; i < count; i++) {
			if (ids[i] == id) {
				popupTime[i] = now;
				return;
			}
		}
	}

	void Clear()
	{
		for (int i = 0; i < MaxSize; i++) {
			ids[i] = 0;
			lastSeen[i] = 0;
			popupTime[i] = 0;
			hasLeft[i] = false;
		}
		count = 0;
	}
};
