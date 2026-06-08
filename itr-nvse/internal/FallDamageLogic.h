#pragma once

#include <cstdint>

namespace FallDamageLogic {

inline float ClampMultiplier(float mult)
{
	return mult < 0.0f ? 0.0f : mult;
}

inline bool StoresActorOverride(float mult)
{
	return mult != 1.0f;
}

template <typename Map>
inline float ResolveMultiplier(std::uint32_t refID, float globalMult, const Map& actorMults)
{
	if (refID && !actorMults.empty())
	{
		auto it = actorMults.find(refID);
		if (it != actorMults.end())
			return it->second;
	}
	return globalMult;
}

}
