//camera override - direct manipulation of camera transform

#include "CameraOverride.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <Windows.h>
#include <cmath>

#include "internal/globals.h"
extern const _ExtractArgs ExtractArgs;

namespace CameraOverride
{
	struct Mat3 {
		float m[3][3];

		void Identity() {
			m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
			m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
			m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
		}

		void RotateX(float rad) {
			float s = sinf(rad), c = cosf(rad);
			m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
			m[1][0] = 0; m[1][1] = c; m[1][2] = s;
			m[2][0] = 0; m[2][1] = -s; m[2][2] = c;
		}

		void RotateY(float rad) {
			float s = sinf(rad), c = cosf(rad);
			m[0][0] = c; m[0][1] = 0; m[0][2] = -s;
			m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
			m[2][0] = s; m[2][1] = 0; m[2][2] = c;
		}

		void RotateZ(float rad) {
			float s = sinf(rad), c = cosf(rad);
			m[0][0] = c; m[0][1] = s; m[0][2] = 0;
			m[1][0] = -s; m[1][1] = c; m[1][2] = 0;
			m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
		}

		Mat3 operator*(const Mat3& b) const {
			Mat3 r;
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					r.m[i][j] = m[i][0]*b.m[0][j] + m[i][1]*b.m[1][j] + m[i][2]*b.m[2][j];
				}
			}
			return r;
		}
	};

	struct Vec3 { float x, y, z; };

	static Mat3 g_rotation;
	static Vec3 g_translation;
	static bool g_overrideRot = false;
	static bool g_overridePos = false;

	static void** g_sceneGraph = (void**)0x11DEB7C;
	static void** g_thePlayer = (void**)0x11DEA3C;

	typedef void* (__thiscall *GetAt_t)(void* node, UInt32 index);
	typedef void (__thiscall *NiAVObjectUpdate_t)(void* node, void* updateData);

	static void* GetCameraRootNode() {
		void* sceneGraph = *g_sceneGraph;
		if (!sceneGraph) return nullptr;
		void** vtable = *(void***)sceneGraph;
		GetAt_t getAt = (GetAt_t)vtable[12];
		return getAt(sceneGraph, 0);
	}

	static bool IsThirdPerson() {
		void* player = *g_thePlayer;
		if (!player) return false;
		return *(bool*)((UInt8*)player + 0x650);
	}

	void Init() {
		g_rotation.Identity();
		g_translation = {0, 0, 0};
	}

	void Update() {
		if (!g_overrideRot && !g_overridePos) return;
		if (!IsThirdPerson()) return;

		void* cameraRoot = GetCameraRootNode();
		if (!cameraRoot) return;

		if (g_overridePos) {
			Vec3* localTranslate = (Vec3*)((UInt8*)cameraRoot + 0x58);
			*localTranslate = g_translation;
		}

		if (g_overrideRot) {
			Mat3* localRotate = (Mat3*)((UInt8*)cameraRoot + 0x34);
			*localRotate = g_rotation;
		}

		void** vtable = *(void***)cameraRoot;
		NiAVObjectUpdate_t updateFunc = (NiAVObjectUpdate_t)vtable[26];
		UInt8 updateData[12] = {0};
		updateFunc(cameraRoot, updateData);
	}

	void SetRotation(bool enable, int axis, float degrees) {
		g_overrideRot = enable;
		if (!enable) {
			g_rotation.Identity();
			return;
		}

		float rad = degrees * 0.01745329252f;
		Mat3 rot;
		switch (axis) {
			case 0: g_rotation.Identity(); return;
			case 1: rot.RotateX(rad); break;
			case 2: rot.RotateY(rad); break;
			case 3: rot.RotateZ(rad); break;
			default: return;
		}
		g_rotation = g_rotation * rot;
	}

	void SetTranslation(bool enable, float x, float y, float z) {
		g_overridePos = enable;
		g_translation = {x, y, z};
	}

	void ResetRotation() {
		g_rotation.Identity();
	}
}

static ParamInfo kParams_SetCameraAngle[4] = {
	{ "enable",  kParamType_Integer, 0 },
	{ "angleX",  kParamType_Float,   0 },
	{ "angleY",  kParamType_Float,   0 },
	{ "angleZ",  kParamType_Float,   0 },
};

DEFINE_COMMAND_PLUGIN(SetCameraAngle, "Sets absolute camera rotation angles (requires JohnnyGuitar)", 0, 4, kParams_SetCameraAngle);

bool Cmd_SetCameraAngle_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 enable = 0;
	float angleX = 0.0f;
	float angleY = 0.0f;
	float angleZ = 0.0f;

	if (!ExtractArgs(EXTRACT_ARGS, &enable, &angleX, &angleY, &angleZ))
		return true;

	CameraOverride::ResetRotation();

	if (enable)
	{
		if (angleX != 0.0f)
			CameraOverride::SetRotation(true, 1, angleX);
		if (angleY != 0.0f)
			CameraOverride::SetRotation(true, 2, angleY);
		if (angleZ != 0.0f)
			CameraOverride::SetRotation(true, 3, angleZ);
	}
	else
	{
		CameraOverride::SetRotation(false, 0, 0);
	}

	if (IsConsoleMode())
	{
		if (enable)
			Console_Print("SetCameraAngle >> Set to X:%.1f Y:%.1f Z:%.1f", angleX, angleY, angleZ);
		else
			Console_Print("SetCameraAngle >> Disabled");
	}

	return true;
}

void CameraOverride_Init()
{
	CameraOverride::Init();
}

void CameraOverride_Update()
{
	CameraOverride::Update();
}

void CameraOverride_RegisterCommands(const void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->RegisterCommand(&kCommandInfo_SetCameraAngle);
}
