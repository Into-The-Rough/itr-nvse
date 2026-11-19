#pragma once
//fixed-size cooldown tracker - extracted from LocationVisitPopup
//tracks items that shouldn't trigger again until cooldown expires and user has "left"

template<int MaxSize>
class CooldownTracker {
public:
	UInt32 ids[MaxSize] = {0};
	UInt32 lastSeen[MaxSize] = {0};
	UInt32 popupTime[MaxSize] = {0};
	bool hasLeft[MaxSize] = {false};
	int count = 0;

	int Capacity() const { return MaxSize; }

	int Count() const { return count; }

	//update all entries - mark as "left" if not seen for leaveThresholdMs
	void UpdateCooldowns(UInt32 now, UInt32 leaveThresholdMs)
	{
		for (int i = 0; i < count; i++)
			if (!hasLeft[i] && now - lastSeen[i] > leaveThresholdMs)
				hasLeft[i] = true;
	}

	//returns 0 to trigger (left and cooldown expired), 1 otherwise
	int Check(UInt32 id, UInt32 now, UInt32 cooldownMs)
	{
		for (int i = 0; i < count; i++) {
			if (ids[i] == id) {
				lastSeen[i] = now;
				if (!hasLeft[i])
					return 1;
				if (popupTime[i] && now - popupTime[i] < cooldownMs)
					return 1;
				//consumed this visit, require another departure before the next trigger
				hasLeft[i] = false;
				return 0;
			}
		}
		//new entry, on a full table evict the entry not seen for the longest
		int slot = count;
		if (count < MaxSize) {
			count++;
		} else {
			slot = 0;
			for (int i = 1; i < MaxSize; i++)
				if (now - lastSeen[i] > now - lastSeen[slot])
					slot = i;
		}
		ids[slot] = id;
		popupTime[slot] = 0;
		hasLeft[slot] = false;
		lastSeen[slot] = now;
		return 1; //first visit, don't trigger
	}

	//mark that popup was shown for this id
	void MarkShown(UInt32 id, UInt32 now)
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
