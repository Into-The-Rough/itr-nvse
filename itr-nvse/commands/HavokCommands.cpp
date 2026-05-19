#include "HavokCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameProcess.h"
#include "nvse/NiObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include <cmath>
#include <cstring>

extern const _ExtractArgs ExtractArgs;

namespace
{
	static constexpr UInt32 kRagdollFlag_Flip = 1;   //spin about pitch axis (front/back flip)
	static constexpr float kRagdollEnterForce = 0.01f;
	//ragdoll bodies don't exist until the anim->ragdoll blend creates them;
	//that can take ~1s, plus KnockExplosion may defer a frame via the task queue
	static constexpr UInt8 kMaxRagdollRetryFrames = 120;
	static constexpr UInt32 kMaxRagdollMotionMs = 60000;
	static constexpr int kMaxRagdollMotions = 64;

	struct Vec3 {
		float x;
		float y;
		float z;
	};

	struct QueuedRagdollMotion {
		UInt32 refID;
		Vec3 linear;
		float spin;
		UInt32 flags;
		DWORD endTime;
		UInt8 limb;
		UInt8 delayFrames;
		UInt8 retryFrames;
		UInt8 repeat;
		UInt8 active;
	};

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

	//bhkWorld::SetMotion(node, motionType, recurse, force, allowActivate) - __cdecl
	typedef int (__cdecl* bhkWorld_SetMotion_t)(void*, int, int, int, int);
	static bhkWorld_SetMotion_t bhkWorld_SetMotion = (bhkWorld_SetMotion_t)0xC6A350;
	static constexpr int kMotionType_Dynamic = 1;

	//bhkUtilFunctions::GetbhkCollisionObject - RTTI-checked, returns null for a
	//node with no bhkCollisionObject. safe when fed real scene nodes.
	typedef void* (__cdecl* GetbhkCollisionObject_t)(void*);
	static GetbhkCollisionObject_t GetbhkCollisionObject = (GetbhkCollisionObject_t)0x43B610;

	static QueuedRagdollMotion s_ragdollMotions[kMaxRagdollMotions] = {};

	bool IsActorRef(TESObjectREFR* ref)
	{
		if (!ref) return false;
		return ThisCall<bool>(*(UInt32*)(*(UInt32*)ref + 0x100), ref);
	}

	float LengthSq(const Vec3& v)
	{
		return v.x * v.x + v.y * v.y + v.z * v.z;
	}

	bool HasMotion(const Vec3& v, float spin)
	{
		return LengthSq(v) > 0.0001f || spin != 0.0f;
	}

	NiNode* GetRefRootNode(TESObjectREFR* ref)
	{
		return ref ? ref->GetNiNode() : nullptr;
	}

	bool IsMobileRigidBody(void* hkpRigidBody)
	{
		UInt8 motionType = *(UInt8*)((UInt8*)hkpRigidBody + 0xE8);
		return (motionType & 2) != 0;
	}

	bool IsRigidBodyActive(void* hkpRigidBody)
	{
		void* simulationIsland = *(void**)((UInt8*)hkpRigidBody + 0xCC);
		if (!simulationIsland)
			return false;

		UInt8 flags = *(UInt8*)((UInt8*)simulationIsland + 0x26);
		UInt8 activeState = (flags >> 2) & 3;
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

	struct NiPoint3 { float x, y, z; };

	//62B660 reads the targeted limb index at limbData+0x10 (1-12 -> biped part,
	//boosts that part's impulse). map the command's limb arg; -1/null = uniform.
	int LimbTargetIndex(UInt8 limb)
	{
		switch (limb)
		{
			case 1:           return 1;   //head
			case 3: case 5:   return 3;   //left arm / hand
			case 4: case 6:   return 5;   //right arm / hand
			case 7: case 9:   return 7;   //left leg / foot
			case 8: case 10:  return 10;  //right leg / foot
			default:          return -1;  //torso / pelvis -> whole-body falloff
		}
	}

	//recurse the real scene tree exactly like NiNode::62B660 does (vtable slot 3
	//= GetAsNiNode, then GetArrayCount/GetAt) and set angular velocity on every
	//ragdoll body via the engine accessors. only safe once the bodies are real
	//(controller exists) - the caller gates on that.
	typedef NiNode* (__thiscall* GetAsNiNode_t)(void*);

	void ApplySpinRecursive(void* node, const float* angVel)
	{
		if (!node)
			return;

		if (void* collObj = GetbhkCollisionObject(node))
		{
			if (void* rb = ThisCall<void*>(0x6FA820, collObj))          //Sun::GetBase
			{
				if (void* hk = ThisCall<void*>(0x4AE750, rb))           //GetHkReferencedObject
					ThisCall<void>(0x561800, hk, angVel);                //setAngularVelocity
			}
		}

		NiNode* asNode = ((GetAsNiNode_t)(*(void***)node)[3])(node);
		if (!asNode)
			return;

		const int count = ThisCall<int>(0x43B480, asNode);              //NiNode::GetArrayCount
		for (int i = 0; i < count; i++)
			ApplySpinRecursive(ThisCall<void*>(0x43B4A0, asNode, i), angVel);  //NiNode::GetAt
	}

	//BIPED_PART (5-bit, bits 8-12 of the collision filter): 1=head, 2=body,
	//3/4=spine, 5/6/7=L upperarm/forearm/hand, 8/9/10=L thigh/calf/foot,
	//11/12/13=R arm, 14/15/16=R leg. command limb arg -> part set; 0 = all.
	bool LimbMatchesPart(UInt8 limb, int part)
	{
		switch (limb)
		{
			case 1:  return part == 1;
			case 2:  return part == 2 || part == 3 || part == 4;
			case 3:  return part >= 5 && part <= 7;
			case 4:  return part >= 11 && part <= 13;
			case 5:  return part == 7;
			case 6:  return part == 13;
			case 7:  return part >= 8 && part <= 10;
			case 8:  return part >= 14 && part <= 16;
			case 9:  return part == 10;
			case 10: return part == 16;
			default: return true;   //whole-body
		}
	}

	//same proven-safe recursion: add a migrated directional impulse to every
	//body whose engine biped-part matches the target limb. assumes the ragdoll
	//controller already exists (caller gates) - no entry, no by-name lookup.
	void ApplyLimbImpulseRecursive(void* node, UInt8 limb, const float* impulse)
	{
		if (!node)
			return;

		if (void* collObj = GetbhkCollisionObject(node))
		{
			if (void* base = ThisCall<void*>(0x6FA820, collObj))         //Sun::GetBase
			{
				UInt32 cf = 0;
				ThisCall<void*>(0x43B4F0, base, &cf);                    //bhkRigidBody::GetCollisionFilter
				const int part = (cf >> 8) & 0x1F;
				if (LimbMatchesPart(limb, part))
				{
					if (void* hk = ThisCall<void*>(0x4AE750, base))      //GetHkReferencedObject
					{
						float* cur = ThisCall<float*>(0x560DC0, hk);     //getLinearVelocity
						alignas(16) float v[4] = {
							impulse[0] + (cur ? cur[0] : 0.0f),
							impulse[1] + (cur ? cur[1] : 0.0f),
							impulse[2] + (cur ? cur[2] : 0.0f),
							0.0f };
						ThisCall<void>(0x5616D0, hk, v);                 //setLinearVelocity
					}
				}
			}
		}

		NiNode* asNode = ((GetAsNiNode_t)(*(void***)node)[3])(node);
		if (!asNode)
			return;

		const int count = ThisCall<int>(0x43B480, asNode);
		for (int i = 0; i < count; i++)
			ApplyLimbImpulseRecursive(ThisCall<void*>(0x43B4A0, asNode, i), limb, impulse);
	}

	//mirrors MiddleHighProcess::KnockExplosion's tail: force the ragdoll bodies
	//dynamic, refresh transforms, then let the engine's own recursive,
	//null-safe NiNode::62B660 apply the migrated directional impulse. spin is
	//layered on after via the same safe recursion. no hand-rolled body lookup.
	bool ApplyRagdollMotion(TESObjectREFR* ref, UInt8 limb, const Vec3& linear, float spin, UInt32 flags)
	{
		NiNode* root = GetRefRootNode(ref);
		if (!root)
			return false;

		bhkWorld_SetMotion(root, kMotionType_Dynamic, 1, 0, 1);

		char arData[0x20] = {};
		ThisCall<void>(0x43D410, arData, 0.0f, 0, 0);   //NiUpdateData::NiUpdateData(0,false,false)
		ThisCall<void>(0xA59C60, root, arData);          //NiAVObject::Update

		//62B660 runs SSE (movaps) on this vector - hkVector4 must be 16-aligned
		NiPoint3 src = { linear.x, linear.y, linear.z };
		alignas(16) float hkDir[4] = {};
		CdeclCall<void*>(0x4A3E00, hkDir, &src);          //NI2HKMIGRATION_POINT3

		const float force = sqrtf(LengthSq(linear));
		const int idx = LimbTargetIndex(limb);
		struct { char pad[0x10]; int index; } limbData = { {}, idx };
		void* limbArg = idx >= 0 ? &limbData : nullptr;

		CdeclCall<void>(0x62B660, root, hkDir, force, limbArg);  //NiNode::62B660

		if (spin != 0.0f)
		{
			//angular velocity (rad/s, scale-invariant - no NI->HK migration).
			//flip flag: spin about the actor's right vector for a head-over-
			//heels front/back flip. rotZ is from +Y clockwise so forward is
			//(sin,cos,0), right is (cos,-sin,0). spin>0 = back, spin<0 = front.
			//otherwise spin about the knock direction (corkscrew), vertical if
			//no direction. one coherent world spin = whole-body tumble.
			Vec3 axis;
			if (flags & kRagdollFlag_Flip)
			{
				const float rz = ref->rotZ;
				axis = { cosf(rz), -sinf(rz), 0.0f };
			}
			else
			{
				axis = linear;
				if (LengthSq(axis) < 0.0001f)
					axis = { 0.0f, 0.0f, 1.0f };
			}
			float len = sqrtf(LengthSq(axis));
			if (len < 0.0001f) len = 1.0f;
			const float s = spin / len;
			alignas(16) float angVel[4] = { axis.x * s, axis.y * s, axis.z * s, 0.0f };
			ApplySpinRecursive(root, angVel);
		}

		return true;
	}

	void PushActorAway(BaseProcess* process, Actor* actor, float x, float y, float z, float force)
	{
		//This virtual is not at one stable slot across process vtables; the
		//implementation itself is stdcall-like thiscall and returns with retn 14h.
		ThisCall<void>(0x91FEE0, process, actor, x, y, z, force);
	}

	UInt8 ClampLimb(int limb)
	{
		if (limb < 0) return 0;
		if (limb > 10) return 10;
		return (UInt8)limb;
	}

	//actor+0xAC is the bhkRagdollController; non-null means the ragdoll instance
	//is built and its bodies are real - the precondition for any Havok access
	bool HasRagdollController(TESObjectREFR* ref)
	{
		return ref && *(void**)((UInt8*)ref + 0xAC) != nullptr;
	}

	bool QueueRagdollMotion(UInt32 refID, UInt8 limb, const Vec3& linear, float spin, float duration, UInt32 flags)
	{
		int slot = -1;
		for (int i = 0; i < kMaxRagdollMotions; i++)
		{
			if (s_ragdollMotions[i].active && s_ragdollMotions[i].refID == refID && s_ragdollMotions[i].limb == limb)
			{
				slot = i;
				break;
			}
			if (!s_ragdollMotions[i].active && slot < 0)
				slot = i;
		}
		if (slot < 0)
			return false;

		DWORD durationMs = 0;
		if (duration > 0.0f)
		{
			double requested = duration * 1000.0;
			if (requested < 1.0)
				requested = 1.0;
			if (requested > kMaxRagdollMotionMs)
				requested = kMaxRagdollMotionMs;
			durationMs = (DWORD)requested;
		}

		QueuedRagdollMotion& motion = s_ragdollMotions[slot];
		motion.refID = refID;
		motion.linear = linear;
		motion.spin = spin;
		motion.flags = flags;
		motion.endTime = durationMs ? GetTickCount() + durationMs : 0;
		motion.limb = limb;
		motion.delayFrames = 1;
		motion.retryFrames = kMaxRagdollRetryFrames;
		motion.repeat = durationMs ? 1 : 0;
		motion.active = 1;
		return true;
	}

	bool GetRigidBodyState(TESObjectREFR* ref, const char* nodeName, bool recurse, HavokObjectRecData& outState)
	{
		if (!ref) return false;
		NiNode* root = GetRefRootNode(ref);
		if (!root) return false;

		NiAVObject* target = root;
		if (nodeName && nodeName[0]) {
			target = static_cast<NiAVObject*>(root->GetObject(nodeName));
			if (!target) return false;
		}

		outState = {};
		outState.recurse = recurse ? 1 : 0;
		outState.action = 8;
		BHKWorld_DoObjectRec(target, &outState, CountRigidBodyState);
		return outState.mobileBodyCount != 0;
	}

	static ParamInfo kParams_IsRigidBodyAtRest[2] = {
		{"nodeName", kParamType_String, 1},
		{"bRecursive", kParamType_Integer, 1},
	};

	static ParamInfo kParams_Ragdoll[7] = {
		{"limb", kParamType_Integer, 1},
		{"x", kParamType_Float, 1},
		{"y", kParamType_Float, 1},
		{"z", kParamType_Float, 1},
		{"spin", kParamType_Float, 1},
		{"duration", kParamType_Float, 1},
		{"flags", kParamType_Integer, 1},
	};

	static ParamInfo kParams_RagdollLimb[4] = {
		{"limb", kParamType_Integer, 0},
		{"x", kParamType_Float, 1},
		{"y", kParamType_Float, 1},
		{"z", kParamType_Float, 1},
	};
}

DEFINE_COMMAND_PLUGIN(IsRigidBodyAtRest, "returns whether a reference's loaded mobile Havok rigid bodies are inactive; optional node name checks a specific rigid body, bRecursive (default 1) includes child bodies", 1, 2, kParams_IsRigidBodyAtRest);
DEFINE_COMMAND_PLUGIN(Ragdoll, "force actor ragdoll; args limb x y z spin duration flags (flags 1 = front/back flip)", 1, 7, kParams_Ragdoll);
DEFINE_COMMAND_PLUGIN(RagdollLimb, "jolt one limb of an already-ragdolling actor; args limb x y z", 1, 4, kParams_RagdollLimb);

bool Cmd_IsRigidBodyAtRest_Execute(COMMAND_ARGS)
{
	*result = 0;
	char nodeName[0x80] = {};
	UInt32 recursive = 1;
	if (!ExtractArgs(EXTRACT_ARGS, &nodeName, &recursive)) return true;

	HavokObjectRecData state = {};
	if (!GetRigidBodyState(thisObj, nodeName, recursive != 0, state))
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

bool Cmd_Ragdoll_Execute(COMMAND_ARGS)
{
	*result = 0;
	int limb = 0;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float spin = 0.0f;
	float duration = 0.0f;
	UInt32 flags = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &limb, &x, &y, &z, &spin, &duration, &flags))
		return true;
	if (!thisObj || !IsActorRef(thisObj))
		return true;

	Actor* actor = (Actor*)thisObj;
	BaseProcess* process = actor->baseProcess;
	if (!process || process->processLevel != 0)   //KnockExplosion is HighProcess-only
		return true;

	//KnockExplosion runs the full engine ragdoll setup (readiness gate,
	//bhkRagdollController create, SetMotion->Dynamic, recursive impulse) and
	//flings the actor away from a world point. that gets the actor ragdolling
	//safely; the queued pass then applies precise per-limb linear+spin once
	//the controller exists.
	const Vec3 linear = { x, y, z };
	const float mag = sqrtf(LengthSq(linear));

	float sx = actor->posX;
	float sy = actor->posY;
	float sz = actor->posZ - 1.0f;
	float force = kRagdollEnterForce;
	if (mag > 0.0001f)
	{
		const float inv = 64.0f / mag;
		sx = actor->posX - linear.x * inv;
		sy = actor->posY - linear.y * inv;
		sz = actor->posZ - linear.z * inv;
		force = mag;
	}

	PushActorAway(process, actor, sx, sy, sz, force);

	const UInt8 targetLimb = ClampLimb(limb);
	if (HasMotion(linear, spin))
		QueueRagdollMotion(actor->refID, targetLimb, linear, spin, duration, flags);

	*result = 1.0;
	return true;
}

bool Cmd_RagdollLimb_Execute(COMMAND_ARGS)
{
	*result = 0;
	int limb = 0;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	if (!ExtractArgs(EXTRACT_ARGS, &limb, &x, &y, &z))
		return true;
	if (!thisObj || !IsActorRef(thisObj))
		return true;
	if (!HasRagdollController(thisObj))   //no-op unless already ragdolling
		return true;

	NiNode* root = GetRefRootNode(thisObj);
	if (!root)
		return true;

	NiPoint3 src = { x, y, z };
	alignas(16) float impulse[4] = {};
	CdeclCall<void*>(0x4A3E00, impulse, &src);   //NI2HKMIGRATION_POINT3

	ApplyLimbImpulseRecursive(root, ClampLimb(limb), impulse);
	*result = 1.0;
	return true;
}

namespace HavokCommands {

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_IsRigidBodyAtRest);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_Ragdoll);
}

void RegisterCommands3(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_RagdollLimb);
}

void ClearState()
{
	memset(s_ragdollMotions, 0, sizeof(s_ragdollMotions));
}

void Update()
{
	const DWORD now = GetTickCount();
	for (auto& motion : s_ragdollMotions)
	{
		if (!motion.active)
			continue;

		auto* ref = (TESObjectREFR*)Engine::LookupFormByID(motion.refID);
		if (!ref || !IsActorRef(ref))
		{
			memset(&motion, 0, sizeof(motion));
			continue;
		}

		//the precondition every prior crash lacked: never touch Havok bodies
		//until the ragdoll controller exists. KnockExplosion builds it within
		//a few frames; wait (don't poke) until then.
		if (!HasRagdollController(ref))
		{
			if (motion.retryFrames)
			{
				motion.retryFrames--;
				continue;
			}
			memset(&motion, 0, sizeof(motion));
			continue;
		}

		ApplyRagdollMotion(ref, motion.limb, motion.linear, motion.spin, motion.flags);

		if (!motion.repeat || now >= motion.endTime)
			memset(&motion, 0, sizeof(motion));
	}
}

}
