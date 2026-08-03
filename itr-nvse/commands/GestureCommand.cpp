//procedural gestures: nod, shake, tilt on the head bone, eyeroll, browraise, squint on the facegen morphs
//head types manipulate Bip01 Head local rotation with smoothstep blend

#include "GestureCommand.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "internal/GestureMath.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "internal/CallTemplates.h"
#include "internal/globals.h"
#include "internal/NiLayout.h"
#include <cstring>

extern const _ExtractArgs ExtractArgs;

namespace GestureCommand
{
	static constexpr float kDegToRad = GestureMath::kPi / 180.0f;
	static constexpr UInt32 kMenuType_Dialogue = 1009;
	static constexpr int MAX_GESTURES = 32;

	struct Gesture
	{
		UInt32 refID = 0;
		UInt8 type = 0; //1=nod, 2=shake, 3=tilt, 4=eyeroll, 5=browraise, 6=squint
		DWORD start = 0;
		DWORD duration = 0;
		float amplitude = 0.0f;
		float cycleTime = 0.4f;
		float animBase[9] = {};
		float lastWritten[9] = {};
		void* headBone = nullptr;
		void* root = nullptr;
		bool hasLast = false;
	};

	static Gesture g_gestures[MAX_GESTURES] = {};

	typedef void* (__cdecl* GetObjectByName_t)(void* rootNode, const char* name);
	static GetObjectByName_t GetObjectByName = (GetObjectByName_t)0x4AAE30;

	typedef void* (__thiscall* GetFaceGenAnimData_t)(void* actor);
	static GetFaceGenAnimData_t GetFaceGenAnimData = (GetFaceGenAnimData_t)0x8ADCB0;

	//a direct SetModifierValue never renders, BSFaceGenNiNode only re-morphs when the anim data reports a change,
	//so morphs go through the keyframe list the blink driver uses
	typedef void (__thiscall* AddModifiersKeyframe_t)(void* faceData, const float* values, float time);
	static AddModifiersKeyframe_t AddModifiersKeyframe = (AddModifiersKeyframe_t)0x64AB40;

	//the list is strictly first in first out, so a pending gesture blocks every later one until it drains.
	//flag order is from the disassembly, not the Xbox PDB signature: arg_8 is the one gating the +0x88 list
	typedef void (__thiscall* ClearKeyframeLists_t)(void* faceData, bool expressionTarget, bool phonemes, bool modifiers, bool custom);
	static ClearKeyframeLists_t ClearKeyframeLists = (ClearKeyframeLists_t)0x64AE60;

	typedef bool (__thiscall* SetKeyframeTime_t)(void* keyframe, float time);
	static SetKeyframeTime_t SetKeyframeTime = (SetKeyframeTime_t)0x64EB30;

	constexpr UInt32 kModifierChannel = 0x94;

	//symmetric min/max eye deflection in radians, from fTrackEyeZ
	typedef void (__cdecl* GetEyeRange_t)(float* outMin, float* outMax);
	static GetEyeRange_t GetEyeRangeVert = (GetEyeRange_t)0x649F70;

	constexpr UInt32 kModifierCount = 17;
	constexpr UInt32 kModifier_BlinkLeft = 0;
	constexpr UInt32 kModifier_BlinkRight = 1;
	constexpr UInt32 kModifier_BrowDownLeft = 2;
	constexpr UInt32 kModifier_BrowDownRight = 3;
	constexpr UInt32 kModifier_BrowUpLeft = 6;
	constexpr UInt32 kModifier_BrowUpRight = 7;
	constexpr UInt32 kModifier_SquintLeft = 12;
	constexpr UInt32 kModifier_SquintRight = 13;

	//BSFaceGenNiNode::UpdateDownwardPass rewrites the Look modifiers every frame from this eye state,
	//so the gesture drives the state and lets the engine convert it
	static float* EyeCurrent(void* faceData) { return (float*)((UInt8*)faceData + 0x140); }
	static float* EyeTarget(void* faceData) { return (float*)((UInt8*)faceData + 0x150); }
	static float* GazeTimer(void* faceData) { return (float*)((UInt8*)faceData + 0x17C); }

	static void* GetRootNode(TESObjectREFR* ref)
	{
		return TESObjectREFRGetNiNodeRaw(ref);
	}

	static void MatMul33(float* out, const float* a, const float* b)
	{
		float tmp[9];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				tmp[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
		memcpy(out, tmp, sizeof(tmp));
	}

	static bool IsNiNode(void* obj)
	{
		if (!obj) return false;
		auto fn = (void*(__thiscall*)(void*))(*((UInt32**)obj))[3];
		return fn(obj) != nullptr;
	}

	static void PropagateTransforms(void* node)
	{
		auto* n = NiAVObjectAsView(node);
		auto* p = NiAVObjectAsView(n->parent);
		if (p)
		{
			float* pwr = p->world.rotate;
			float* lr = n->local.rotate;
			float* wr = n->world.rotate;
			MatMul33(wr, pwr, lr);

			float ps = p->world.scale;
			float sx = n->local.translate[0] * ps;
			float sy = n->local.translate[1] * ps;
			float sz = n->local.translate[2] * ps;
			n->world.translate[0] = p->world.translate[0] + pwr[0] * sx + pwr[1] * sy + pwr[2] * sz;
			n->world.translate[1] = p->world.translate[1] + pwr[3] * sx + pwr[4] * sy + pwr[5] * sz;
			n->world.translate[2] = p->world.translate[2] + pwr[6] * sx + pwr[7] * sy + pwr[8] * sz;
			n->world.scale = ps * n->local.scale;
		}

		if (IsNiNode(node))
		{
			UInt16 count = NiNodeGetChildLimit(node);
			void** children = NiNodeGetChildData(node);
			if (children && count > 0 && count < 512)
				for (UInt16 i = 0; i < count; i++)
					if (children[i]) PropagateTransforms(children[i]);
		}
	}

	static void* FindHeadBone(TESObjectREFR* ref)
	{
		void* root = GetRootNode(ref);
		if (!root)
			return nullptr;

		return GetObjectByName(root, "Bip01 Head");
	}

	static void ClearGesture(Gesture& g)
	{
		memset(&g, 0, sizeof(g));
	}

	static void RestoreGesturePose(Gesture& g)
	{
		if (!g.hasLast)
			return;

		auto* form = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(g.refID));
		if (!form)
			return;

		void* headBone = FindHeadBone(form);
		if (!headBone)
			return;

		//only unwind our own write, if the animation system rewrote the bone it already owns the pose
		float* localRot = NiAVObjectAsView(headBone)->local.rotate;
		if (memcmp(localRot, g.lastWritten, sizeof(g.lastWritten)) != 0)
			return;
		GestureMath::CopyMat3(localRot, g.animBase);
		PropagateTransforms(headBone);
	}

	//the engine range checks morph weights to 0..1 (see SetExpressionTarget), past that they stop applying
	static float ClampWeight(float value)
	{
		return (value <= 0.0f || value > 1.0f) ? 1.0f : value;
	}

	//a keyframe carries all 17 modifiers, FLT_MAX means leave that one to whoever else drives it,
	//which is how the blink driver touches only the eyelids
	constexpr float kModifierUnset = 3.4028235e38f;
	constexpr float kMaxHoldSlices = 64.0f;

	//ramp in, hold, ramp back to rest, the engine interpolates between the keyframes and reports the change
	static bool PushModifierKeyframes(TESObjectREFR* actor, const UInt32* modifiers, const float* weights, UInt32 count, float duration)
	{
		void* faceData = GetFaceGenAnimData(actor);
		if (!faceData)
			return false;

		//drop whatever is still queued, otherwise this gesture waits behind it and the drain reads as a flicker
		ClearKeyframeLists(faceData, false, false, true, false);
		//the channel only zeroes its clock on a frame where it sees the list empty, and clearing then pushing in
		//one go never gives it that frame, so stale time would drain the whole sequence before it ever renders
		SetKeyframeTime((UInt8*)faceData + kModifierChannel, 0.0f);

		float ramp = 0.15f;
		if (duration < 0.5f)
			ramp = duration * 0.3f;
		float hold = duration - ramp * 2.0f;
		if (hold < 0.0f)
			hold = 0.0f;

		float values[kModifierCount];
		float rest[kModifierCount];
		for (UInt32 i = 0; i < kModifierCount; i++)
		{
			values[i] = kModifierUnset;
			rest[i] = kModifierUnset;
		}
		for (UInt32 i = 0; i < count; i++)
		{
			values[modifiers[i]] = weights[i];
			rest[modifiers[i]] = 0.0f;
		}

		AddModifiersKeyframe(faceData, values, ramp);

		//the mesh is only re-morphed on frames where the anim data reports a change, so a dead flat hold stops
		//being applied, the 3 percent wobble keeps it reporting dirty
		float wobble[kModifierCount];
		for (UInt32 i = 0; i < kModifierCount; i++)
			wobble[i] = values[i];
		for (UInt32 i = 0; i < count; i++)
			wobble[modifiers[i]] = weights[i] * 0.97f;

		//each slice is a heap allocated keyframe, so long holds stretch the slice instead of queueing thousands
		float step = 0.15f;
		if (hold > step * kMaxHoldSlices)
			step = hold / kMaxHoldSlices;

		float remaining = hold;
		bool low = true;
		while (remaining > 0.0f)
		{
			const float slice = (remaining < step) ? remaining : step;
			AddModifiersKeyframe(faceData, low ? wobble : values, slice);
			remaining -= slice;
			low = !low;
		}

		AddModifiersKeyframe(faceData, rest, ramp);
		return true;
	}

	static void StopFacial(void* faceData)
	{
		//hand the eyes back centred, the engine eases them there and resumes its own gaze
		float* target = EyeTarget(faceData);
		target[0] = 0.0f;
		target[1] = 0.0f;
	}

	static void StopGesture(Gesture& g)
	{
		if (GestureMath::IsFacialGesture(g.type))
		{
			auto* form = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(g.refID));
			if (form && Engine::TESObjectREFR_IsActor(form))
			{
				if (void* faceData = GetFaceGenAnimData(form))
					StopFacial(faceData);
			}
		}
		else
			RestoreGesturePose(g);

		ClearGesture(g);
	}

	static void UpdateFacial(Gesture& g, TESObjectREFR* form, DWORD now)
	{
		if (!Engine::TESObjectREFR_IsActor(form)) { ClearGesture(g); return; }

		void* faceData = GetFaceGenAnimData(form);
		if (!faceData) { ClearGesture(g); return; }

		if (!g.start) g.start = now;
		DWORD elapsed = now - g.start;
		if (elapsed >= g.duration) { StopFacial(faceData); ClearGesture(g); return; }

		//an eye roll is a look up, held, then released, the envelope shapes all three
		const float weight = GestureMath::ComputeMorphWeight(g.type, elapsed, g.duration, g.amplitude, g.cycleTime);

		float vertMin, vertMax;
		GetEyeRangeVert(&vertMin, &vertMax);
		const float y = weight * vertMax;

		float* current = EyeCurrent(faceData);
		float* target = EyeTarget(faceData);
		current[0] = 0.0f;
		current[1] = y;
		target[0] = 0.0f;
		target[1] = y;
		//hold off the idle saccade while the gesture owns the eyes
		*GazeTimer(faceData) = 1.0f;
	}

	void Init() {}

	//load/new-game only, the cached pose belongs to the previous session's skeleton so drop
	//it without restoring onto the freshly loaded bones
	void Reset()
	{
		for (auto& g : g_gestures)
			ClearGesture(g);
	}

	void Update()
	{
		//defer in menus other than dialogue
		if (CdeclCall<bool>(0x702360) && !IsMenuVisible(kMenuType_Dialogue)) return;

		DWORD now = GetTickCount();

		for (int i = 0; i < MAX_GESTURES; i++)
		{
			auto& g = g_gestures[i];
			if (!g.type) continue;

			auto* form = (TESObjectREFR*)Engine::LookupFormByID(g.refID);
			if (!form) { ClearGesture(g); continue; }

			if (GestureMath::IsFacialGesture(g.type)) { UpdateFacial(g, form, now); continue; }

			//re-scan for the bone only when the skeleton root changes, the name lookup is the costly part
			void* root = GetRootNode(form);
			if (!root) { ClearGesture(g); continue; }
			if (root != g.root)
			{
				g.root = root;
				g.headBone = GetObjectByName(root, "Bip01 Head");
				g.hasLast = false;
			}

			void* headBone = g.headBone;
			if (!headBone) { ClearGesture(g); continue; }

			//rebase whenever the animation system rewrote the bone since our last write,
			//so the gesture rides on top of headtracking instead of a stale snapshot
			float* localRot = NiAVObjectAsView(headBone)->local.rotate;
			if (!g.hasLast || memcmp(localRot, g.lastWritten, sizeof(g.lastWritten)) != 0)
				GestureMath::CopyMat3(g.animBase, localRot);

			if (!g.start) g.start = now;
			DWORD elapsed = now - g.start;
			if (elapsed >= g.duration) { StopGesture(g); continue; }

			float angle = GestureMath::ComputeAngleRadians(g.type, elapsed, g.duration, g.amplitude, g.cycleTime);
			GestureMath::ComposePoseFromBase(g.animBase, g.type, angle, localRot);
			GestureMath::CopyMat3(g.lastWritten, localRot);
			g.hasLast = true;

			PropagateTransforms(headBone);
		}
	}

	static bool StartGesture(TESObjectREFR* actor, UInt8 type, float amplitude, float duration, float speed)
	{
		if (duration <= 0.0f)
			return false;

		//brow and squint are pure morph animations, the keyframe list plays them without a slot
		if (type == GestureMath::kGesture_BrowRaise || type == GestureMath::kGesture_Squint)
		{
			const float weight = ClampWeight(amplitude);
			if (type == GestureMath::kGesture_Squint)
			{
				//the brows coming down are what read as a squint, the lid narrowing alone barely shows
				const UInt32 squint[] = { kModifier_SquintLeft, kModifier_SquintRight, kModifier_BrowDownLeft, kModifier_BrowDownRight };
				const float weights[] = { weight, weight, weight, weight };
				return PushModifierKeyframes(actor, squint, weights, 4, duration);
			}

			const UInt32 brows[] = { kModifier_BrowUpLeft, kModifier_BrowUpRight };
			const float weights[] = { weight, weight };
			return PushModifierKeyframes(actor, brows, weights, 2, duration);
		}

		const UInt32 refID = actor->refID;

		//find existing or free slot
		int slot = -1;
		for (int i = 0; i < MAX_GESTURES; i++)
		{
			if (g_gestures[i].refID == refID) { slot = i; break; }
			if (!g_gestures[i].type && slot < 0) slot = i;
		}
		if (slot < 0) return false;

		auto& g = g_gestures[slot];
		//a facial gesture owns its morph values until it stops, so hand them back before the slot is reused
		if (g.type && GestureMath::IsFacialGesture(g.type))
			StopGesture(g);
		else if (g.refID != refID)
			ClearGesture(g);

		if (speed <= 0.0f)
			speed = 0.4f;

		g.refID = refID;
		g.type = type;
		g.start = 0;
		g.duration = (DWORD)(duration * 1000.0f);
		if (!g.duration)
			g.duration = 1;
		g.amplitude = GestureMath::IsFacialGesture(type) ? ClampWeight(amplitude) : amplitude * kDegToRad;
		g.cycleTime = speed;

		//20 degrees of eye travel is all the engine allows, the raised brows and lowered lids are what make it read
		if (type == GestureMath::kGesture_EyeRoll)
		{
			const UInt32 modifiers[] = { kModifier_BrowUpLeft, kModifier_BrowUpRight, kModifier_BlinkLeft, kModifier_BlinkRight };
			const float brow = g.amplitude;
			const float lid = g.amplitude * 0.35f;
			const float weights[] = { brow, brow, lid, lid };
			PushModifierKeyframes(actor, modifiers, weights, 4, duration);
		}
		return true;
	}

	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_Gesture);
	}
}

static ParamInfo kParams_Gesture[] = {
	{"type", kParamType_String, 0},
	{"amplitude", kParamType_Float, 0},
	{"duration", kParamType_Float, 0},
	{"speed", kParamType_Float, 1},
};

bool Cmd_Gesture_Execute(COMMAND_ARGS)
{
	*result = 0;
	char type[512] = {};
	float amplitude = 8.0f;
	float duration = 3.0f;
	float speed = 0.4f;

	if (!ExtractArgs(EXTRACT_ARGS, &type, &amplitude, &duration, &speed))
		return true;
	if (!Engine::TESObjectREFR_IsActor(thisObj)) return true;

	UInt8 gestureType = 0;
	if (_stricmp(type, "nod") == 0) gestureType = 1;
	else if (_stricmp(type, "shake") == 0) gestureType = 2;
	else if (_stricmp(type, "tilt") == 0) gestureType = 3;
	else if (_stricmp(type, "eyeroll") == 0) gestureType = 4;
	else if (_stricmp(type, "browraise") == 0) gestureType = 5;
	else if (_stricmp(type, "squint") == 0) gestureType = 6;
	if (!gestureType) return true;

	if (GestureCommand::StartGesture(thisObj, gestureType, amplitude, duration, speed))
		*result = 1;
	return true;
}

CommandInfo kCommandInfo_Gesture = {
	"Gesture", "", 0, "play a procedural head or facial gesture on an actor",
	1, 4, kParams_Gesture, Cmd_Gesture_Execute, nullptr, nullptr, 0
};
