#include "AimZoomFirstPersonOnly.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameSDK.h"
#include "internal/layout/Player.h"
#include "internal/globals.h"
#include "internal/settings.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/PluginAPI.h"

extern const _ExtractArgs ExtractArgs;

namespace AimZoomFirstPersonOnly
{
	using UpdateAimZoom_t = void(__thiscall*)(void* player, float delta);

	//both sites call the aim-zoom update at 0x95DE30, from PlayerCharacter::Update and the secondary update path
	constexpr UInt32 kCall_PlayerUpdateAimZoom = 0x94375E;
	constexpr UInt32 kCall_SecondaryUpdateAimZoom = 0x944847;

	static bool s_installAttempted = false;
	static Detours::CallDetour s_playerUpdateCall;
	static Detours::CallDetour s_secondaryUpdateCall;
	static UpdateAimZoom_t s_playerUpdateOriginal = nullptr;
	static UpdateAimZoom_t s_secondaryUpdateOriginal = nullptr;

	static bool IsThirdPerson(void* player)
	{
		return PlayerCharacterIsAimZoomThirdPerson(static_cast<PlayerCharacter*>(player));
	}

	static bool IsAiming(void* player)
	{
		return player && ThisCall<bool>(0x8BBC10, player);
	}

	static bool ShouldSuppressAimZoom(void* player)
	{
		return Settings::bAimZoomFirstPersonOnly && IsThirdPerson(player) && IsAiming(player);
	}

	static void ResetAimZoomToDefaults(void* player)
	{
		if (!player)
			return;

		auto* defaultWorldFOV = Engine::GetSettingFloatPtr(g_fDefaultWorldFOVSetting);
		auto* defaultFirstPersonFOV = Engine::GetSettingFloatPtr(g_fDefaultFirstPersonFOVSetting);
		if (!defaultWorldFOV || !defaultFirstPersonFOV)
			return;

		PlayerCharacterSetFOVs(static_cast<PlayerCharacter*>(player), *defaultWorldFOV, *defaultFirstPersonFOV);
	}

	static void CallOriginal(UpdateAimZoom_t original, void* player, float delta)
	{
		if (!original)
			return;

		const bool suppressAimZoom = ShouldSuppressAimZoom(player);
		if (!suppressAimZoom)
		{
			original(player, delta);
			return;
		}

		auto* ironSightsZoomDefault = Engine::GetSettingFloatPtr(g_fIronSightsZoomDefaultSetting);
		if (!ironSightsZoomDefault)
		{
			original(player, delta);
			ResetAimZoomToDefaults(player);
			return;
		}

		const float savedIronSightsZoomDefault = *ironSightsZoomDefault;
		*ironSightsZoomDefault = 0.0f;
		original(player, delta);
		*ironSightsZoomDefault = savedIronSightsZoomDefault;
		ResetAimZoomToDefaults(player);
	}

	static void __fastcall Hook_UpdateAimZoomFromPlayerUpdate(void* player, void*, float delta)
	{
		CallOriginal(s_playerUpdateOriginal, player, delta);
	}

	static void __fastcall Hook_UpdateAimZoomFromSecondaryUpdate(void* player, void*, float delta)
	{
		CallOriginal(s_secondaryUpdateOriginal, player, delta);
	}

	static bool InstallHook()
	{
		if (s_installAttempted)
			return s_playerUpdateOriginal || s_secondaryUpdateOriginal;

		s_installAttempted = true;

		if (s_playerUpdateCall.WriteRelCall(kCall_PlayerUpdateAimZoom, Hook_UpdateAimZoomFromPlayerUpdate))
			s_playerUpdateOriginal = reinterpret_cast<UpdateAimZoom_t>(s_playerUpdateCall.GetOverwrittenAddr());
		else
			Log("AimZoomFirstPersonOnly: failed to hook PlayerCharacter::Update aim zoom call");

		if (s_secondaryUpdateCall.WriteRelCall(kCall_SecondaryUpdateAimZoom, Hook_UpdateAimZoomFromSecondaryUpdate))
			s_secondaryUpdateOriginal = reinterpret_cast<UpdateAimZoom_t>(s_secondaryUpdateCall.GetOverwrittenAddr());
		else
			Log("AimZoomFirstPersonOnly: failed to hook secondary aim zoom call");

		return s_playerUpdateOriginal || s_secondaryUpdateOriginal;
	}

	void Init(bool enabled)
	{
		Settings::bAimZoomFirstPersonOnly = enabled ? 1 : 0;
		InstallHook();
	}

	void SetEnabled(bool enabled)
	{
		Settings::bAimZoomFirstPersonOnly = enabled ? 1 : 0;
		if (!s_playerUpdateOriginal && !s_secondaryUpdateOriginal)
			InstallHook();
	}

	bool IsEnabled()
	{
		return Settings::bAimZoomFirstPersonOnly != 0;
	}
}

static ParamInfo kParams_SetAimZoomFirstPersonOnly[1] = {
	{ "enable", kParamType_Integer, 0 },
};

DEFINE_COMMAND_PLUGIN(SetAimZoomFirstPersonOnly, "Enable or disable first-person-only aiming zoom", 0, 1, kParams_SetAimZoomFirstPersonOnly);
DEFINE_COMMAND_PLUGIN(GetAimZoomFirstPersonOnly, "Returns whether aiming zoom is limited to first person", 0, 0, NULL);

bool Cmd_SetAimZoomFirstPersonOnly_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 enable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &enable))
		return true;

	AimZoomFirstPersonOnly::SetEnabled(enable != 0);
	*result = AimZoomFirstPersonOnly::IsEnabled() ? 1.0 : 0.0;

	if (IsConsoleMode())
		Console_Print("AimZoomFirstPersonOnly >> %s", enable ? "enabled" : "disabled");

	return true;
}

bool Cmd_GetAimZoomFirstPersonOnly_Execute(COMMAND_ARGS)
{
	*result = AimZoomFirstPersonOnly::IsEnabled() ? 1.0 : 0.0;
	return true;
}

namespace AimZoomFirstPersonOnly
{
	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_SetAimZoomFirstPersonOnly);
		nvse->RegisterCommand(&kCommandInfo_GetAimZoomFirstPersonOnly);
	}
}
