//shared witness scan used by both GetWitnesses command and OnWitnessed trespass hook
//iterates nearby actors in the perpetrator's cell (plus the 3x3 exterior grid) and filters by detection threshold
#pragma once

#include <vector>

#include "common/ITypes.h"

class Actor;

namespace WitnessScan {
	struct Hit {
		Actor* actor;
		int    detectionValue;
		UInt32 refID;
	};

	//if crimeLoc is null, uses perpetrator position
	//radius and detectionThreshold 0 = use configured defaults
	void FindWitnesses(Actor* perpetrator, const float* crimeLocXYZ,
	                   float radius, int detectionThreshold,
	                   std::vector<Hit>& out);

	//single-actor query - returns DetectionData.detectionValue, or -100 if none
	int GetDetectionValue(Actor* observer, Actor* target);
}
