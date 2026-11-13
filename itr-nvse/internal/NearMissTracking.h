#pragma once

#include <cstdint>
#include <cstring>

namespace NearMissTracking {
	struct Point3 {
		float x, y, z;
	};

	class FlightHistory {
		static constexpr uint32_t kCapacity = 1024;
		static constexpr uint32_t kProbes = 4;

		struct Flight {
			uint32_t id;
			uint32_t stamp;
			float lifeTime;
			Point3 pos;
		};

		Flight flights[kCapacity] = {};
		uint32_t stamp = 0;

	public:
		Point3 Observe(uint32_t id, float lifeBefore, float lifeAfter, const Point3& before, const Point3& after)
		{
			Point3 start = before;
			const uint32_t first = (id ^ (id >> 10)) % kCapacity;
			auto* slot = &flights[first];
			++stamp;
			for (uint32_t i = 0; i < kProbes; ++i) {
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
