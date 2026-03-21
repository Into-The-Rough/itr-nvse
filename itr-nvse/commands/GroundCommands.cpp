//MoveToGround / GetDistanceToGround
//uses TESObjectCELL::GetHeightAtPos (heightmap query, not raycast)

#include "GroundCommands.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <vector>
#include <cmath>

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

namespace
{
	struct MoveEntry {
		UInt32 refID;
		float speed;
	};

	static std::vector<MoveEntry> s_pendingMoves;

	bool GetGroundZ(TESObjectREFR* ref, float* outZ)
	{
		auto* cell = *(TESObjectCELL**)(((UInt8*)ref) + 0x40);
		if (!cell) return false;
		float xy[2] = { ref->posX, ref->posY };
		return ThisCall<bool>(0x5547C0, cell, xy, outZ);
	}
}

//ref.MoveToGround speed:float
bool Cmd_MoveToGround_Execute(COMMAND_ARGS)
{
	*result = 0;
	float speed = 0;
	ExtractArgs(EXTRACT_ARGS, &speed);

	if (!thisObj) return true;

	float groundZ;
	if (!GetGroundZ(thisObj, &groundZ))
		return true;

	if (speed <= 0) {
		ThisCall<void>(0x575B70, thisObj, groundZ);
		*result = 1;
		return true;
	}

	//remove any existing entry for this ref
	UInt32 refID = thisObj->refID;
	for (auto it = s_pendingMoves.begin(); it != s_pendingMoves.end(); ++it) {
		if (it->refID == refID) {
			s_pendingMoves.erase(it);
			break;
		}
	}

	s_pendingMoves.push_back({ refID, speed });
	*result = 1;
	return true;
}

//ref.GetDistanceToGround
bool Cmd_GetDistanceToGround_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!thisObj) return true;

	float groundZ;
	if (!GetGroundZ(thisObj, &groundZ))
		return true;

	*result = thisObj->posZ - groundZ;
	return true;
}

static ParamInfo kParams_OneOptionalFloat[] = {
	{ "speed", kParamType_Float, 1 },
};

DEFINE_COMMAND_PLUGIN(MoveToGround, "move reference to ground surface, optional speed in units/sec (0=instant)", true, 1, kParams_OneOptionalFloat);
DEFINE_COMMAND_PLUGIN(GetDistanceToGround, "get Z distance from reference to ground below", true, 0, nullptr);

namespace GroundCommands
{
	void Update()
	{
		if (s_pendingMoves.empty()) return;

		float dt = *(float*)0x11F63A0; //TimeGlobal->secondsPassed
		if (dt <= 0) return;

		for (int i = (int)s_pendingMoves.size() - 1; i >= 0; i--) {
			auto& entry = s_pendingMoves[i];
			auto* ref = (TESObjectREFR*)Engine::LookupFormByID(entry.refID);
			if (!ref || ref->IsDeleted()) {
				s_pendingMoves.erase(s_pendingMoves.begin() + i);
				continue;
			}

			float groundZ;
			if (!GetGroundZ(ref, &groundZ)) {
				s_pendingMoves.erase(s_pendingMoves.begin() + i);
				continue;
			}

			float curZ = ref->posZ;
			float diff = groundZ - curZ;
			float step = entry.speed * dt;

			if (fabsf(diff) <= step) {
				ThisCall<void>(0x575B70, ref, groundZ);
				s_pendingMoves.erase(s_pendingMoves.begin() + i);
			} else {
				float newZ = curZ + (diff > 0 ? step : -step);
				ThisCall<void>(0x575B70, ref, newZ);
			}
		}
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_MoveToGround);
		nvse->RegisterCommand(&kCommandInfo_GetDistanceToGround);
	}

	void ClearState()
	{
		s_pendingMoves.clear();
	}

	bool Init(void* nvse)
	{
		return true;
	}
}
