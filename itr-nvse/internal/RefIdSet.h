#pragma once
//fixed-size set of refIDs - used for NoWeaponSearch and similar features

template<int MaxSize>
class RefIdSet {
public:
	UInt32 ids[MaxSize] = {0};
	int count = 0;

	bool Contains(UInt32 refID) const
	{
		for (int i = 0; i < count; i++)
			if (ids[i] == refID)
				return true;
		return false;
	}

	bool Add(UInt32 refID)
	{
		if (refID == 0) return false;
		if (Contains(refID)) return false;
		if (count >= MaxSize) return false;
		ids[count++] = refID;
		return true;
	}

	bool Remove(UInt32 refID)
	{
		for (int i = 0; i < count; i++)
		{
			if (ids[i] == refID)
			{
				ids[i] = ids[--count];
				ids[count] = 0;
				return true;
			}
		}
		return false;
	}

	void Clear()
	{
		for (int i = 0; i < count; i++)
			ids[i] = 0;
		count = 0;
	}

	int Count() const { return count; }
	int Capacity() const { return MaxSize; }
};
