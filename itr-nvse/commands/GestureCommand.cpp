//procedural head gestures: nod, shake, tilt
//manipulates Bip01 Head local rotation with smoothstep blend

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
	static constexpr int MAX_GESTURES = 32;

	struct Gesture
	{
		UInt32 refID = 0;
		UInt8 type = 0; //1=nod, 2=shake, 3=tilt
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

	static void StopGesture(Gesture& g)
	{
		RestoreGesturePose(g);
		ClearGesture(g);
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
		//defer until gamemode
		if (CdeclCall<bool>(0x702360)) return;

		DWORD now = GetTickCount();
		for (int i = 0; i < MAX_GESTURES; i++)
		{
			auto& g = g_gestures[i];
			if (!g.type) continue;

			auto* form = (TESObjectREFR*)Engine::LookupFormByID(g.refID);
			if (!form) { ClearGesture(g); continue; }

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

	static bool StartGesture(UInt32 refID, UInt8 type, float amplitude, float duration, float speed)
	{
		//find existing or free slot
		int slot = -1;
		for (int i = 0; i < MAX_GESTURES; i++)
		{
			if (g_gestures[i].refID == refID) { slot = i; break; }
			if (!g_gestures[i].type && slot < 0) slot = i;
		}
		if (slot < 0) return false;

		auto& g = g_gestures[slot];
		if (g.refID != refID)
			ClearGesture(g);

		if (duration <= 0.0f)
			return false;
		if (speed <= 0.0f)
			speed = 0.4f;

		g.refID = refID;
		g.type = type;
		g.start = 0;
		g.duration = (DWORD)(duration * 1000.0f);
		if (!g.duration)
			g.duration = 1;
		g.amplitude = amplitude * kDegToRad;
		g.cycleTime = speed;
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
	if (!gestureType) return true;

	if (GestureCommand::StartGesture(thisObj->refID, gestureType, amplitude, duration, speed))
		*result = 1;
	return true;
}

CommandInfo kCommandInfo_Gesture = {
	"Gesture", "", 0, "play a procedural head gesture on an actor",
	1, 4, kParams_Gesture, Cmd_Gesture_Execute, nullptr, nullptr, 0
};
