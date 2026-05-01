#include "HavokCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/NiObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;

namespace
{
	struct HavokObjectRecData {
		void* world;
		UInt8 recurse;
		UInt8 pad05[3];
		UInt32 action;
		UInt32 mobileBodyCount;
		UInt32 activeBodyCount;
	};

	typedef void (__cdecl* BHKWorld_DoObjectRec_t)(NiAVObject* object, HavokObjectRecData* data,
		void (__cdecl* callback)(void* collisionObject, HavokObjectRecData* data));

	static BHKWorld_DoObjectRec_t BHKWorld_DoObjectRec = (BHKWorld_DoObjectRec_t)0xC68900;

	bool IsMobileRigidBody(void* hkpRigidBody)
	{
		if (!hkpRigidBody) return false;
		UInt8 motionType = *(UInt8*)((UInt8*)hkpRigidBody + 0xE8);
		return (motionType & 2) != 0;
	}

	bool IsRigidBodyActive(void* hkpRigidBody)
	{
		if (!hkpRigidBody) return false;
		void* simulationIsland = *(void**)((UInt8*)hkpRigidBody + 0xCC);
		if (!simulationIsland) return false;
		UInt8 activeState = (*(UInt8*)((UInt8*)simulationIsland + 0x26) >> 2) & 3;
		return activeState != 0;
	}

	void __cdecl CountRigidBodyState(void* collisionObject, HavokObjectRecData* data)
	{
		if (!collisionObject || !data) return;

		void* worldObject = *(void**)((UInt8*)collisionObject + 0x10);
		if (!worldObject) return;

		void* hkpObject = *(void**)((UInt8*)worldObject + 0x08);
		if (!hkpObject) return;
		if (*(UInt8*)((UInt8*)hkpObject + 0x28) != 1)
			return;
		if (!IsMobileRigidBody(hkpObject))
			return;

		data->mobileBodyCount++;
		if (IsRigidBodyActive(hkpObject))
			data->activeBodyCount++;
	}

	bool GetRigidBodyState(TESObjectREFR* ref, const char* nodeName, HavokObjectRecData& outState)
	{
		if (!ref) return false;
		NiNode* root = ref->GetNiNode();
		if (!root) return false;

		NiAVObject* target = root;
		if (nodeName && nodeName[0]) {
			target = static_cast<NiAVObject*>(root->GetObject(nodeName));
			if (!target) return false;
		}

		outState = {};
		outState.recurse = 1;
		outState.action = 8;
		BHKWorld_DoObjectRec(target, &outState, CountRigidBodyState);
		return outState.mobileBodyCount != 0;
	}

	static ParamInfo kParams_IsRigidBodyAtRest[1] = {
		{"nodeName", kParamType_String, 1},
	};
}

DEFINE_COMMAND_PLUGIN(IsRigidBodyAtRest, "returns whether a reference's loaded mobile Havok rigid bodies are inactive", 1, 1, kParams_IsRigidBodyAtRest);

bool Cmd_IsRigidBodyAtRest_Execute(COMMAND_ARGS)
{
	*result = 0;
	char nodeName[0x80] = {};
	if (!ExtractArgs(EXTRACT_ARGS, &nodeName)) return true;

	HavokObjectRecData state = {};
	if (!GetRigidBodyState(thisObj, nodeName, state))
		return true;

	*result = (state.activeBodyCount == 0) ? 1.0 : 0.0;
	if (IsConsoleMode()) {
		Console_Print("IsRigidBodyAtRest >> %s mobile=%u active=%u",
			*result != 0.0 ? "resting" : "active",
			state.mobileBodyCount,
			state.activeBodyCount);
	}
	return true;
}

namespace HavokCommands {

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_IsRigidBodyAtRest);
}

}
