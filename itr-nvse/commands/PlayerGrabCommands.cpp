#include "PlayerGrabCommands.h"

#include <cmath>

#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/EngineHelpers.h"
#include "internal/GameGlobals.h"
#include "internal/HavokLayout.h"
#include "internal/PlayerGrabLogic.h"
#include "internal/RayCast.h"
#include "internal/globals.h"
#include "internal/layout/Player.h"

#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "nvse/PluginAPI.h"

extern const _ExtractArgs ExtractArgs;
extern bool g_isLoadingSave;

namespace {
	struct NiUpdateData {
		float timePassed;
		bool updateControllers;
		bool isMultiThreaded;
		UInt8 byte06;
		bool updateGeomorphs;
		bool updateShadowScene;
		UInt8 pad09[3];
	};

	struct PendingGrab {
		UInt32 refID;
		float distance;
		UInt8 reposition;
		UInt8 retryFrames;
		UInt8 skipSameFrame;
		UInt8 active;
	};

	enum AttachResult : UInt8 {
		kAttach_Failed = 0,
		kAttach_Attached = 1,
		kAttach_Pending3D = 2,
		kAttach_DeferNextFrame = 3,
	};

	constexpr UInt32 kGrabType_ZKey = 1;
	constexpr UInt32 kControl_Grab = 27;
	constexpr UInt8 kKeyQuery_IsPressed = 1;
	constexpr UInt32 kExtraData_Action = 0x7C;
	constexpr UInt8 kMaxPendingGrabFrames = 60;
	constexpr float kGrabMaxRayDist = 1000.0f;

	using CreateMouseSpring_t = void(__thiscall*)(PlayerCharacter*, TESObjectREFR*, UInt32, float);
	using DestroyMouseSpring_t = void(__thiscall*)(PlayerCharacter*);
	using GetEyeVector_t = void(__thiscall*)(Actor*, PlayerGrabLogic::Point3*, PlayerGrabLogic::Point3*, bool);
	using SetLocationOnRef_t = void(__thiscall*)(TESObjectREFR*, const PlayerGrabLogic::Point3*);
	using SetLocalTranslate_t = void(__thiscall*)(NiNode*, const PlayerGrabLogic::Point3*);
	using NiAVObjectUpdate_t = void(__thiscall*)(NiNode*, NiUpdateData*);
	using ResetSim_t = void(__cdecl*)(NiNode*, bool);
	using SetRigidBodyVelocity_t = void(__thiscall*)(void*, const float*);
	using ActivateEntity_t = void(__thiscall*)(void*);
	using FireExtraAction_t = void*(__thiscall*)(BaseExtraList*, TESObjectREFR*, Actor*);

	const auto PlayerCharacter_CreateMouseSpring = reinterpret_cast<CreateMouseSpring_t>(0x95F930);
	const auto PlayerCharacter_DestroyMouseSpring = reinterpret_cast<DestroyMouseSpring_t>(0x961280);
	const auto Actor_GetEyeVector = reinterpret_cast<GetEyeVector_t>(0x8AFF80);
	const auto TESObjectREFR_SetLocationOnRef = reinterpret_cast<SetLocationOnRef_t>(0x575830);
	const auto NiNode_SetLocalTranslate = reinterpret_cast<SetLocalTranslate_t>(0x440460);
	const auto NiAVObject_Update = reinterpret_cast<NiAVObjectUpdate_t>(0xA59C60);
	const auto BhkNiCollisionObject_ResetSim = reinterpret_cast<ResetSim_t>(0xC6BD00);
	const auto HkpRigidBody_SetLinearVelocity = reinterpret_cast<SetRigidBodyVelocity_t>(0x5616D0);
	const auto HkpRigidBody_SetAngularVelocity = reinterpret_cast<SetRigidBodyVelocity_t>(0x561800);
	const auto HkpEntity_Activate = reinterpret_cast<ActivateEntity_t>(0xC9C1D0);
	const auto ExtraDataList_FireGrabAction = reinterpret_cast<FireExtraAction_t>(0x4213C0);

	PendingGrab s_pendingGrab = {};

	bool AllowGrabbingLiveActors()
	{
		UInt8* flag = Engine::GetSettingUInt8Ptr(g_bAllowGrabbingLiveActorsSetting);
		return flag && *flag != 0;
	}

	bool IsValidGrabTarget(PlayerCharacter* player, TESObjectREFR* ref)
	{
		if (!player || !ref || ref == player || g_isLoadingSave || IsGameLoading())
			return false;
		if (!ref->IsReference())
			return false;
		if (Engine::TESForm_GetDeleted(ref) || Engine::TESForm_GetDisabled(ref))
			return false;
		if (!ref->parentCell || !player->parentCell)
			return false;
		if (Engine::TESObjectREFR_IsActor(ref) && !AllowGrabbingLiveActors() &&
			!Engine::Actor_IsDead(static_cast<Actor*>(ref), false))
			return false;
		return true;
	}

	PlayerGrabLogic::Point3 ComputeWorldHoldPoint(PlayerCharacter* player, const PlayerGrabLogic::Point3& eyePos,
		const PlayerGrabLogic::Point3& eyeDir, float requestedDistance, void* ignoreHkpBody)
	{
		const float desiredDist = PlayerGrabLogic::ClampRequestedDistance(requestedDistance);
		const float rayLength = desiredDist + PlayerGrabLogic::kWallClearance;
		const PlayerGrabLogic::Point3 rayEnd = PlayerGrabLogic::PointAlongRay(eyePos, eyeDir, rayLength);

		RayCastData ray = {};
		ray.pos0[0] = eyePos.x * kHavokScale;
		ray.pos0[1] = eyePos.y * kHavokScale;
		ray.pos0[2] = eyePos.z * kHavokScale;
		ray.pos1[0] = rayEnd.x * kHavokScale;
		ray.pos1[1] = rayEnd.y * kHavokScale;
		ray.pos1[2] = rayEnd.z * kHavokScale;
		ray.hitFraction = 1.0f;
		ray.unk44[0] = 0xFFFFFFFF;
		ray.unk44[6] = 0xFFFFFFFF;
		ray.layerType = kRayLayer_Projectile;
		ray.group = ActorGetCollisionGroup(player);

		float holdDist = desiredDist;
		if (Engine::TESPickObject(&ray, true) && !ray.budgetSpent && ray.cdBody && ray.hitFraction < 1.0f) {
			if (!ignoreHkpBody || HkpWorldObjectFromCollidableRoot(ray.cdBody) != ignoreHkpBody)
				holdDist = PlayerGrabLogic::ClampHoldDistanceForWall(desiredDist, rayLength, ray.hitFraction);
		}
		return PlayerGrabLogic::PointAlongRay(eyePos, eyeDir, holdDist);
	}

	void SyncRefAndBodyToPos(TESObjectREFR* ref, NiNode* root, void* hkpBody, const PlayerGrabLogic::Point3& pos)
	{
		TESObjectREFR_SetLocationOnRef(ref, &pos);
		if (!root)
			return;

		NiNode_SetLocalTranslate(root, &pos);
		BhkNiCollisionObject_ResetSim(root, true);
		NiUpdateData updateData = {};
		NiAVObject_Update(root, &updateData);

		if (hkpBody && HkpRigidBodyIsMobile(hkpBody)) {
			alignas(16) const float zeroVel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			HkpRigidBody_SetLinearVelocity(hkpBody, zeroVel);
			HkpRigidBody_SetAngularVelocity(hkpBody, zeroVel);
			//0x5616D0 skips 0xC9C1D0 when velocity is already zero
			HkpEntity_Activate(hkpBody);
		}
	}

	bool DestroyActiveGrab(PlayerCharacter* player)
	{
		if (!player)
			return false;
		PlayerGrabView* grab = PlayerCharacterGetGrabView(player);
		const bool hadGrab = grab->grabSpring != nullptr || grab->grabbedRef != nullptr || *g_playerGrabActivePtr != 0;
		//clear before 0x961280 so a reentrant ForcePlayerGrab inside OnRelease keeps its active flag
		*g_playerGrabActivePtr = 0;
		if (grab->grabSpring || grab->grabbedRef)
			PlayerCharacter_DestroyMouseSpring(player);
		return hadGrab;
	}

	AttachResult AttachGrabNow(UInt32 refID, float requestedDistance, bool forceReposition)
	{
		PlayerCharacter* player = *g_thePlayerPtr;
		TESObjectREFR* ref = LookupRefByID(refID);
		if (!IsValidGrabTarget(player, ref))
			return kAttach_Failed;

		PlayerGrabView* grab = PlayerCharacterGetGrabView(player);
		if (grab->grabSpring || grab->grabbedRef) {
			DestroyActiveGrab(player);
			player = *g_thePlayerPtr;
			ref = LookupRefByID(refID);
			if (!IsValidGrabTarget(player, ref))
				return kAttach_Failed;
			grab = PlayerCharacterGetGrabView(player);
			if (grab->grabSpring || grab->grabbedRef || s_pendingGrab.active)
				return kAttach_Failed;
		}

		NiNode* root = ref->GetNiNode();
		void* bhkBody = root ? Engine::GetHeaviestRigidBody(root) : nullptr;
		void* hkpBody = BhkWorldObjectGetHavokObject(bhkBody);
		if (!root || !bhkBody || !hkpBody || !HkpWorldObjectAsView(hkpBody)->world)
			return kAttach_Pending3D;

		if (!Engine::TESObjectREFR_IsActor(ref) && !HkpRigidBodyIsMobile(hkpBody))
			return kAttach_Failed;

		//if Z was pressed this frame before UpdateGrab (0x95F6C0), wait 1 frame so UpdateGrab does not toggle-drop
		if (*g_inputGlobalsPtr && Engine::OSInputGlobals_GetControlState(*g_inputGlobalsPtr, kControl_Grab, kKeyQuery_IsPressed))
			return kAttach_DeferNextFrame;

		PlayerGrabLogic::Point3 eyePos = {};
		PlayerGrabLogic::Point3 eyeDir = {};
		Actor_GetEyeVector(player, &eyePos, &eyeDir, true);

		const PlayerGrabLogic::Point3 refPos = {ref->posX, ref->posY, ref->posZ};
		if (PlayerGrabLogic::ShouldRepositionForGrab(eyePos, eyeDir, refPos, forceReposition)) {
			const PlayerGrabLogic::Point3 holdPos = ComputeWorldHoldPoint(player, eyePos, eyeDir, requestedDistance, hkpBody);
			SyncRefAndBodyToPos(ref, root, hkpBody, holdPos);
		}

		//0x95F930 hijacks the spring onto dword_11CC5EC when the crosshair rests on a corpse limb
		void* savedCrosshairBody = *g_crosshairGrabBodyPtr;
		*g_crosshairGrabBodyPtr = nullptr;

		PlayerCharacter_CreateMouseSpring(player, ref, kGrabType_ZKey, kGrabMaxRayDist);
		if (!*g_crosshairGrabBodyPtr)
			*g_crosshairGrabBodyPtr = savedCrosshairBody;

		//re-resolve after 0x95F930 in case OnGrab mutated player grab state or deleted the ref
		player = *g_thePlayerPtr;
		if (!player)
			return kAttach_Failed;
		grab = PlayerCharacterGetGrabView(player);
		if (!grab->grabSpring || !grab->grabbedRef)
			return kAttach_Failed;

		TESObjectREFR* postRef = LookupRefByID(refID);
		if (!postRef || grab->grabbedRef != postRef)
			return kAttach_Failed;

		*g_playerGrabActivePtr = 1;
		postRef->MarkAsModified(4);
		ExtraDataList_FireGrabAction(&postRef->extraDataList, postRef, player);
		postRef = LookupRefByID(refID);
		if (postRef)
			Engine::BaseExtraList_RemoveByType(&postRef->extraDataList, kExtraData_Action);
		return kAttach_Attached;
	}

	static ParamInfo kParams_ForcePlayerGrab[1] = {
		{"distance", kParamType_Float, 1},
	};
}

DEFINE_COMMAND_PLUGIN(ForcePlayerGrab, "pulls the calling reference into the player's physics grab hold, deferring if its 3D/Havok body is still attaching", 1, 1, kParams_ForcePlayerGrab);
DEFINE_COMMAND_PLUGIN(ReleasePlayerGrab, "releases any active or pending player physics grab", 0, 0, nullptr);

bool Cmd_ForcePlayerGrab_Execute(COMMAND_ARGS)
{
	*result = 0;
	float distance = 0.0f;
	if (!ExtractArgs(EXTRACT_ARGS, &distance))
		return true;

	PlayerCharacter* player = *g_thePlayerPtr;
	if (!IsValidGrabTarget(player, thisObj))
		return true;

	const UInt32 refID = thisObj->refID;
	s_pendingGrab = {};

	const AttachResult res = AttachGrabNow(refID, distance, distance > 0.0f);
	if (res == kAttach_Attached) {
		*result = 1;
		if (IsConsoleMode())
			Console_Print("ForcePlayerGrab >> 1 (attached)");
		return true;
	}

	if (res == kAttach_Pending3D || res == kAttach_DeferNextFrame) {
		if (s_pendingGrab.active) {
			*result = 1;
			return true;
		}
		player = *g_thePlayerPtr;
		TESObjectREFR* ref = LookupRefByID(refID);
		if (!IsValidGrabTarget(player, ref))
			return true;

		if (res == kAttach_Pending3D) {
			PlayerGrabLogic::Point3 eyePos = {};
			PlayerGrabLogic::Point3 eyeDir = {};
			Actor_GetEyeVector(player, &eyePos, &eyeDir, true);
			const PlayerGrabLogic::Point3 holdPos = ComputeWorldHoldPoint(player, eyePos, eyeDir, distance, nullptr);
			//set ref coords only - updating an unattached cached NiNode strands its rigid body outside bhkWorld
			TESObjectREFR_SetLocationOnRef(ref, &holdPos);
		}

		s_pendingGrab.refID = refID;
		s_pendingGrab.distance = distance;
		s_pendingGrab.reposition = (res == kAttach_Pending3D || distance > 0.0f) ? 1 : 0;
		s_pendingGrab.retryFrames = kMaxPendingGrabFrames;
		s_pendingGrab.skipSameFrame = 1;
		s_pendingGrab.active = 1;
		*result = 1;
		if (IsConsoleMode())
			Console_Print("ForcePlayerGrab >> 1 (queued)");
		return true;
	}

	if (IsConsoleMode())
		Console_Print("ForcePlayerGrab >> 0");
	return true;
}

bool Cmd_ReleasePlayerGrab_Execute(COMMAND_ARGS)
{
	const bool hadPending = s_pendingGrab.active != 0;
	s_pendingGrab = {};

	const bool hadActive = DestroyActiveGrab(*g_thePlayerPtr);
	*result = (hadPending || hadActive) ? 1 : 0;
	if (IsConsoleMode())
		Console_Print("ReleasePlayerGrab >> %d", (*result != 0) ? 1 : 0);
	return true;
}

namespace PlayerGrabCommands {
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_ForcePlayerGrab);
		nvse->RegisterCommand(&kCommandInfo_ReleasePlayerGrab);
	}

	void Update()
	{
		if (!s_pendingGrab.active)
			return;
		if (s_pendingGrab.skipSameFrame) {
			s_pendingGrab.skipSameFrame = 0;
			return;
		}

		const PendingGrab pending = s_pendingGrab;
		s_pendingGrab = {};

		const AttachResult res = AttachGrabNow(pending.refID, pending.distance, pending.reposition != 0);
		if (res == kAttach_Attached)
			return;
		if ((res == kAttach_Pending3D || res == kAttach_DeferNextFrame) && !s_pendingGrab.active && pending.retryFrames > 1) {
			s_pendingGrab.refID = pending.refID;
			s_pendingGrab.distance = pending.distance;
			s_pendingGrab.reposition = pending.reposition;
			s_pendingGrab.retryFrames = static_cast<UInt8>(pending.retryFrames - 1);
			s_pendingGrab.active = 1;
			return;
		}
	}

	void ClearState()
	{
		s_pendingGrab = {};
	}
}
