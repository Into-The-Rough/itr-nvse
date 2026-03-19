//camera override - hooks engine camera update via CameraHooks

#include "CameraOverride.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <Windows.h>
#include <cmath>

#include "internal/globals.h"
#include "internal/Mat3.h"
#include "handlers/DialogueCameraHandler.h"
extern const _ExtractArgs ExtractArgs;

namespace CameraOverride
{
	static Mat3 g_rotation;
	static bool g_overrideRot = false;

	void SetRotation(bool enable, int axis, float degrees) {
		g_overrideRot = enable;
		if (!enable) {
			g_rotation.Identity();
			DialogueCameraHandler::ClearExternalRotation();
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
		DialogueCameraHandler::SetExternalRotation(g_rotation);
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

DEFINE_COMMAND_PLUGIN(SetCameraAngle, "Sets absolute camera rotation angles", 0, 4, kParams_SetCameraAngle);

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

namespace CameraOverride {
void Init()
{
	g_rotation.Identity();
}

void RegisterCommands(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->RegisterCommand(&kCommandInfo_SetCameraAngle);
}
}
