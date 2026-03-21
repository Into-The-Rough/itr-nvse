//MoveToGround / GetDistanceToGround
//uses TESObjectCELL::GetHeightAtPos (heightmap query, not raycast)

#include "GroundCommands.h"
#include "internal/CallTemplates.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <cmath>

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

namespace
{
	bool GetGroundZ(TESObjectREFR* ref, float* outZ)
	{
		auto* cell = *(TESObjectCELL**)(((UInt8*)ref) + 0x40);
		if (!cell) return false;
		float xy[2] = { ref->posX, ref->posY };
		return ThisCall<bool>(0x5547C0, cell, xy, outZ);
	}

	//SetLocationOnReference + NiNode translate + NiAVObject::Update
	void SetPosAndUpdate3D(TESObjectREFR* ref, float newZ)
	{
		float pos[3] = { ref->posX, ref->posY, newZ };
		ThisCall<void>(0x575830, ref, pos); //SetLocationOnReference

		auto* renderState = *(void**)(((UInt8*)ref) + 0x64);
		if (!renderState) return;
		auto* niNode = *(void**)(((UInt8*)renderState) + 0x14);
		if (!niNode) return;

		//NiNode local translate at +0x58 (x,y) +0x60 (z)
		*(float*)(((UInt8*)niNode) + 0x58) = ref->posX;
		*(float*)(((UInt8*)niNode) + 0x5C) = ref->posY;
		*(float*)(((UInt8*)niNode) + 0x60) = newZ;

		//NiAVObject::Update(NiUpdateData*)
		static float s_niUpdateData[2] = { 0.0f, 0.0f };
		ThisCall<void>(0xA5DD70, niNode, s_niUpdateData, 0);
	}
}

//ref.MoveToGround distance:float
bool Cmd_MoveToGround_Execute(COMMAND_ARGS)
{
	*result = 0;
	float distance = 0;
	ExtractArgs(EXTRACT_ARGS, &distance);

	if (!thisObj) return true;

	float groundZ;
	if (!GetGroundZ(thisObj, &groundZ))
		return true;

	float actualDist = thisObj->posZ - groundZ;
	if (actualDist <= 0) return true; //already at or below ground

	if (distance <= 0) {
		//instant clamp
		SetPosAndUpdate3D(thisObj, groundZ);
	} else {
		//move down by distance, capped at ground
		float move = (distance < actualDist) ? distance : actualDist;
		SetPosAndUpdate3D(thisObj, thisObj->posZ - move);
	}

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
	{ "distance", kParamType_Float, 1 },
};

DEFINE_COMMAND_PLUGIN(MoveToGround, "move reference toward ground, 0=instant, >0=units to descend (capped at ground)", true, 1, kParams_OneOptionalFloat);
DEFINE_COMMAND_PLUGIN(GetDistanceToGround, "get Z distance from reference to ground below", true, 0, nullptr);

namespace GroundCommands
{
	void Update() {}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_MoveToGround);
		nvse->RegisterCommand(&kCommandInfo_GetDistanceToGround);
	}

	void ClearState() {}

	bool Init(void* nvse)
	{
		return true;
	}
}
