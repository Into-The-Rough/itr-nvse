#pragma once

#include <cstdint>

namespace OnWitnessedLogic {

inline std::uint32_t ResolveWitnessID(std::uint32_t countBefore,
	std::uint32_t countAfter, std::uint32_t actorID, std::uint32_t criminalID,
	std::uint32_t targetID, bool isPickpocket, bool isMurder)
{
	if (countAfter <= countBefore || !actorID)
		return 0;

	if (isPickpocket && actorID == criminalID)
		actorID = targetID;

	if (!actorID || (isMurder && actorID == targetID))
		return 0;

	return actorID;
}

}
