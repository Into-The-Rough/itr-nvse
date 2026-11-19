#pragma once
#include <cstring>

namespace NearMissTracking {
	struct Point3 {
		float x, y, z;
	};

	class FlightHistory {
		static constexpr UInt32 kCapacity = 1024;
		static constexpr UInt32 kProbes = 4;

		struct Flight {
			UInt32 id;
			UInt32 stamp;
			float lifeTime;
			Point3 pos;
		};

		Flight flights[kCapacity] = {};
		UInt32 stamp = 0;

	public:
		Point3 Observe(UInt32 id, float lifeBefore, float lifeAfter, const Point3& before, const Point3& after)
		{
			Point3 start = before;
			const UInt32 first = (id ^ (id >> 10)) % kCapacity;
			auto* slot = &flights[first];
			++stamp;
			for (UInt32 i = 0; i < kProbes; ++i) {
				auto* candidate = &flights[(first + i) % kCapacity];
				if (candidate->id == id || !candidate->id) {
					slot = candidate;
					break;
				}
				if (stamp - candidate->stamp > stamp - slot->stamp)
					slot = candidate;
			}
			if (id && slot->id == id && lifeBefore > 0.0f && slot->lifeTime == lifeBefore)
				start = slot->pos;
			*slot = {id, stamp, lifeAfter, after};
			return start;
		}

		void Clear()
		{
			std::memset(flights, 0, sizeof(flights));
			stamp = 0;
		}
	};
}
