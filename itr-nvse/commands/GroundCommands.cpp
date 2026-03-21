//MoveToTerrain / GetDistanceToTerrain / MoveToGround / GetDistanceToGround
//terrain commands use GetHeightAtPos; ground commands use a downward raycast with terrain fallback

#include "GroundCommands.h"
#include "internal/CallTemplates.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

namespace
{
	struct NiPoint3 {
		float x;
		float y;
		float z;
	};

	struct NiUpdateData {
		float timePassed;
		bool updateControllers;
		bool isMultiThreaded;
		UInt8 byte06;
		bool updateGeomorphs;
		bool updateShadowScene;
		UInt8 pad09[3];
	};

	struct alignas(16) RayCastData {
		float pos0[4];
		float pos1[4];
		UInt8 byte20;
		UInt8 pad21[3];
		UInt8 layerType;
		UInt8 filterFlags;
		UInt16 group;
		UInt32 unk28[6];
		float hitFraction;
		UInt32 unk44[15];
		void* cdBody;
		UInt32 unk84[3];
		float vector90[4];
		UInt32 unkA0[3];
		UInt8 byteAC;
		UInt8 padAD[3];
	};
	static_assert(sizeof(RayCastData) == 0xB0, "RayCastData size mismatch");

	constexpr float kHavokScale = 0.1428571f;
	constexpr float kGroundRayMaxRange = 50000.0f;
	constexpr float kGroundRayStartOffset = 8.0f;
	constexpr UInt8 kGroundRayLayer = 6;

	typedef void (__thiscall *TES_PickObject_t)(void* tes, RayCastData* rcData, bool unk);
	static TES_PickObject_t TES_PickObject = (TES_PickObject_t)0x458440;
	static void** g_TES = (void**)0x11DEA10;

	bool GetTerrainZ(TESObjectREFR* ref, float* outZ)
	{
		auto* cell = *(TESObjectCELL**)(((UInt8*)ref) + 0x40);
		if (!cell) return false;
		float xy[2] = { ref->posX, ref->posY };
		return ThisCall<bool>(0x5547C0, cell, xy, outZ);
	}

	bool GetRaycastGroundZ(TESObjectREFR* ref, float* outZ)
	{
		if (!*g_TES) return false;

		float startZ = ref->posZ + kGroundRayStartOffset;
		float endZ = startZ - kGroundRayMaxRange;

		RayCastData rcData = {};
		rcData.pos0[0] = ref->posX * kHavokScale;
		rcData.pos0[1] = ref->posY * kHavokScale;
		rcData.pos0[2] = startZ * kHavokScale;
		rcData.pos1[0] = ref->posX * kHavokScale;
		rcData.pos1[1] = ref->posY * kHavokScale;
		rcData.pos1[2] = endZ * kHavokScale;
		rcData.hitFraction = 1.0f;
		rcData.unk44[0] = 0xFFFFFFFF;
		rcData.unk44[6] = 0xFFFFFFFF;
		rcData.layerType = kGroundRayLayer;

		TES_PickObject(*g_TES, &rcData, true);
		if (rcData.hitFraction >= 1.0f)
			return false;

		*outZ = startZ + ((endZ - startZ) * rcData.hitFraction);
		return true;
	}

	bool GetGroundZ(TESObjectREFR* ref, float* outZ)
	{
		if (GetRaycastGroundZ(ref, outZ))
			return true;
		return GetTerrainZ(ref, outZ);
	}

	//Mirror the engine's position update path so the ref's 3D and collision stay in sync.
	void SetPosAndUpdate3D(TESObjectREFR* ref, float newZ)
	{
		NiPoint3 pos = { ref->posX, ref->posY, newZ };
		ThisCall<void>(0x575830, ref, &pos); //SetLocationOnReference

		auto* renderState = ref->renderState;
		if (!renderState || !renderState->niNode)
			return;

		auto* niNode = renderState->niNode;
		ThisCall<void>(0x440460, niNode, &pos); //NiAVObject::SetLocalTranslate
		CdeclCall<void>(0xC6BD00, niNode, true); //bhkNiCollisionObject::ResetSim
		ThisCall<void>(0xA5A040, niNode); //NiAVObject::UpdateProperties

		NiUpdateData updateData = {};
		ThisCall<void>(0xA5DD70, niNode, &updateData, 0); //NiNode::UpdateDownwardPass
	}

	void MoveRefTowardZ(TESObjectREFR* ref, float targetZ, float distance)
	{
		float actualDist = ref->posZ - targetZ;
		if (actualDist <= 0)
			return;

		if (distance <= 0) {
			SetPosAndUpdate3D(ref, targetZ);
		} else {
			float move = (distance < actualDist) ? distance : actualDist;
			SetPosAndUpdate3D(ref, ref->posZ - move);
		}
	}
}

//ref.MoveToTerrain distance:float
bool Cmd_MoveToTerrain_Execute(COMMAND_ARGS)
{
	*result = 0;
	float distance = 0;
	ExtractArgs(EXTRACT_ARGS, &distance);

	if (!thisObj) return true;

	float terrainZ;
	if (!GetTerrainZ(thisObj, &terrainZ))
		return true;

	MoveRefTowardZ(thisObj, terrainZ, distance);
	*result = 1;
	return true;
}

//ref.GetDistanceToTerrain
bool Cmd_GetDistanceToTerrain_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!thisObj) return true;

	float terrainZ;
	if (!GetTerrainZ(thisObj, &terrainZ))
		return true;

	*result = thisObj->posZ - terrainZ;
	return true;
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

	MoveRefTowardZ(thisObj, groundZ, distance);
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

DEFINE_COMMAND_PLUGIN(MoveToTerrain, "move reference toward terrain below, 0=instant, >0=units to descend (capped at terrain)", true, 1, kParams_OneOptionalFloat);
DEFINE_COMMAND_PLUGIN(GetDistanceToTerrain, "get Z distance from reference to terrain below", true, 0, nullptr);
DEFINE_COMMAND_PLUGIN(MoveToGround, "move reference toward ground below (raycast, terrain fallback), 0=instant, >0=units to descend (capped at ground)", true, 1, kParams_OneOptionalFloat);
DEFINE_COMMAND_PLUGIN(GetDistanceToGround, "get Z distance from reference to ground below (raycast, terrain fallback)", true, 0, nullptr);

namespace GroundCommands
{
	void Update() {}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_MoveToTerrain);
		nvse->RegisterCommand(&kCommandInfo_GetDistanceToTerrain);
		nvse->RegisterCommand(&kCommandInfo_MoveToGround);
		nvse->RegisterCommand(&kCommandInfo_GetDistanceToGround);
	}

	void ClearState() {}

	bool Init(void* nvse)
	{
		return true;
	}
}
