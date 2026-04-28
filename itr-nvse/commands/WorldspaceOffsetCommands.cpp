#include "WorldspaceOffsetCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameForms.h"

extern const _ExtractArgs ExtractArgs;

namespace
{
	enum WorldspaceOffsetComponent
	{
		kWorldspaceOffset_X,
		kWorldspaceOffset_Y,
		kWorldspaceOffset_Scale,
	};

	static double GetOffsetValue(TESWorldSpace* world, WorldspaceOffsetComponent component)
	{
		if (!world)
			return 0.0;

		switch (component)
		{
			case kWorldspaceOffset_X:
				return world->worldMapCellX;
			case kWorldspaceOffset_Y:
				return world->worldMapCellY;
			case kWorldspaceOffset_Scale:
				return world->worldMapScale;
		}

		return 0.0;
	}

	static bool GetWorldspaceOffset_Execute(COMMAND_ARGS, WorldspaceOffsetComponent component)
	{
		*result = 0.0;

		TESWorldSpace* world = nullptr;
		if (!ExtractArgs(EXTRACT_ARGS, &world))
			return true;

		*result = GetOffsetValue(world, component);
		return true;
	}

	static ParamInfo kParams_OneWorldspace[1] =
	{
		{ "worldspace", kParamType_WorldSpace, 0 },
	};

	DEFINE_COMMAND_PLUGIN(GetWorldspaceOffsetX, "returns a worldspace's map X offset", 0, 1, kParams_OneWorldspace);
	DEFINE_COMMAND_PLUGIN(GetWorldspaceOffsetY, "returns a worldspace's map Y offset", 0, 1, kParams_OneWorldspace);
	DEFINE_COMMAND_PLUGIN(GetWorldspaceOffsetScale, "returns a worldspace's map scale", 0, 1, kParams_OneWorldspace);

	bool Cmd_GetWorldspaceOffsetX_Execute(COMMAND_ARGS)
	{
		return GetWorldspaceOffset_Execute(PASS_COMMAND_ARGS, kWorldspaceOffset_X);
	}

	bool Cmd_GetWorldspaceOffsetY_Execute(COMMAND_ARGS)
	{
		return GetWorldspaceOffset_Execute(PASS_COMMAND_ARGS, kWorldspaceOffset_Y);
	}

	bool Cmd_GetWorldspaceOffsetScale_Execute(COMMAND_ARGS)
	{
		return GetWorldspaceOffset_Execute(PASS_COMMAND_ARGS, kWorldspaceOffset_Scale);
	}
}

namespace WorldspaceOffsetCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_GetWorldspaceOffsetX);
		nvse->RegisterCommand(&kCommandInfo_GetWorldspaceOffsetY);
		nvse->RegisterCommand(&kCommandInfo_GetWorldspaceOffsetScale);
	}
}
