//procedural head gestures: nod, shake, tilt
//manipulates Bip01 Head local rotation with smoothstep blend

#include "GestureCommand.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "internal/GestureMath.h"
#include "internal/EngineFunctions.h"
#include "internal/CallTemplates.h"
#include "internal/globals.h"
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
		float baseRot[9] = {};
		void* headBone = nullptr;
		bool hasBaseRot = false;
	};

	static Gesture g_gestures[MAX_GESTURES] = {};

	typedef void* (__cdecl* GetObjectByName_t)(void* rootNode, const char* name);
	static GetObjectByName_t GetObjectByName = (GetObjectByName_t)0x4AAE30;

	static void* GetRootNode(TESObjectREFR* ref)
	{
		void* renderData = *(void**)((UInt8*)ref + 0x64);
		if (!renderData) return nullptr;
		return *(void**)((UInt8*)renderData + 0x14);
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
		auto* n = (UInt8*)node;
		auto* p = *(UInt8**)(n + 0x18);
		if (p)
		{
			float* pwr = (float*)(p + 0x68);
			float* lr = (float*)(n + 0x34);
			float* wr = (float*)(n + 0x68);
			MatMul33(wr, pwr, lr);

			float ps = *(float*)(p + 0x98);
			float sx = *(float*)(n + 0x58) * ps;
			float sy = *(float*)(n + 0x5C) * ps;
			float sz = *(float*)(n + 0x60) * ps;
			*(float*)(n + 0x8C) = *(float*)(p + 0x8C) + pwr[0] * sx + pwr[1] * sy + pwr[2] * sz;
			*(float*)(n + 0x90) = *(float*)(p + 0x90) + pwr[3] * sx + pwr[4] * sy + pwr[5] * sz;
			*(float*)(n + 0x94) = *(float*)(p + 0x94) + pwr[6] * sx + pwr[7] * sy + pwr[8] * sz;
			*(float*)(n + 0x98) = ps * *(float*)(n + 0x64);
		}

		if (IsNiNode(node))
		{
			UInt16 count = *(UInt16*)(n + 0xA6);
			auto** children = *(void***)(n + 0xA0);
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
		if (!g.hasBaseRot)
			return;

		auto* form = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(g.refID));
		if (!form)
			return;

		void* headBone = FindHeadBone(form);
		if (!headBone)
			return;

		float* localRot = reinterpret_cast<float*>(reinterpret_cast<UInt8*>(headBone) + 0x34);
		GestureMath::CopyMat3(localRot, g.baseRot);
		PropagateTransforms(headBone);
	}

	static void StopGesture(Gesture& g)
	{
		RestoreGesturePose(g);
		ClearGesture(g);
	}

	void Init() {}

	void Reset()
	{
		for (auto& g : g_gestures)
		{
			if (g.type)
				StopGesture(g);
		}
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

			void* headBone = FindHeadBone(form);
			if (!headBone) { ClearGesture(g); continue; }

			if (g.headBone != headBone)
			{
				g.headBone = headBone;
				g.hasBaseRot = false;
			}

			float* localRot = (float*)((UInt8*)headBone + 0x34);
			if (!g.hasBaseRot)
			{
				GestureMath::CopyMat3(g.baseRot, localRot);
				g.hasBaseRot = true;
			}

			if (!g.start) g.start = now;
			DWORD elapsed = now - g.start;
			if (elapsed >= g.duration) { StopGesture(g); continue; }

			float angle = GestureMath::ComputeAngleRadians(g.type, elapsed, g.duration, g.amplitude, g.cycleTime);
			GestureMath::ComposePoseFromBase(g.baseRot, g.type, angle, localRot);

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

inline bool IsActorRef(TESObjectREFR* ref) {
	if (!ref) return false;
	return ThisCall<bool>(*(UInt32*)(*(UInt32*)ref + 0x100), ref);
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
	if (!thisObj || !IsActorRef(thisObj)) return true;

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
