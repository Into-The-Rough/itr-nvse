#include "CommandBoundsCommand.h"

#ifdef _DEBUG

#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"

#include "features/NoWeaponSearch.h"
#include "features/PreventWeaponSwitch.h"
#include "handlers/FallDamageHandler.h"
#include "handlers/OnContactHandler.h"
#include "handlers/DialogueCameraHandler.h"
#include "internal/globals.h"

#include <cmath>
#include <cstdio>

extern const _ExtractArgs ExtractArgs;

namespace {
	enum class CaseStatus {
		kPass,
		kFail,
		kSkip,
	};

	struct CaseResult {
		CaseStatus status;
		const char* detail;
	};

	using CaseFn = CaseResult (*)(int mode, PlayerCharacter* player);

	struct CaseDef {
		const char* name;
		int minMode;
		CaseFn fn;
	};

	constexpr float kFloatEpsilon = 0.001f;

	static bool NearlyEqual(float a, float b)
	{
		return std::fabs(a - b) <= kFloatEpsilon;
	}

	static CaseResult Pass(const char* detail = "")
	{
		return { CaseStatus::kPass, detail };
	}

	static CaseResult Fail(const char* detail)
	{
		return { CaseStatus::kFail, detail };
	}

	static CaseResult Skip(const char* detail)
	{
		return { CaseStatus::kSkip, detail };
	}

	static void ReportCase(const char* name, const CaseResult& result)
	{
		const char* status = "PASS";
		switch (result.status)
		{
			case CaseStatus::kFail:
				status = "FAIL";
				break;
			case CaseStatus::kSkip:
				status = "SKIP";
				break;
			default:
				break;
		}

		char buffer[256] = {};
		if (result.detail && result.detail[0])
			snprintf(buffer, sizeof(buffer), "RunITRCommandBounds >> %s %s (%s)", status, name, result.detail);
		else
			snprintf(buffer, sizeof(buffer), "RunITRCommandBounds >> %s %s", status, name);

		Log("%s", buffer);
		if (IsConsoleMode())
			Console_Print("%s", buffer);
	}

	static CaseResult Case_FallDamage_Global(int, PlayerCharacter*)
	{
		float originalGlobal = FallDamageHandler::GetMultiplier();
		FallDamageHandler::SetMultiplier(-2.0f);
		if (!NearlyEqual(FallDamageHandler::GetMultiplier(), 0.0f))
		{
			FallDamageHandler::SetMultiplier(originalGlobal);
			return Fail("negative clamp");
		}

		FallDamageHandler::SetMultiplier(1.75f);
		if (!NearlyEqual(FallDamageHandler::GetMultiplier(), 1.75f))
		{
			FallDamageHandler::SetMultiplier(originalGlobal);
			return Fail("set/get mismatch");
		}

		FallDamageHandler::SetMultiplier(originalGlobal);
		if (!NearlyEqual(FallDamageHandler::GetMultiplier(), originalGlobal))
			return Fail("restore mismatch");

		return Pass();
	}

	static CaseResult Case_FallDamage_Actor(int, PlayerCharacter* player)
	{
		if (!player)
			return Skip("no player");

		bool hadOverride = FallDamageHandler::HasOverride(player);
		float originalEffective = FallDamageHandler::GetMultiplier(player);
		float globalValue = FallDamageHandler::GetMultiplier();

		FallDamageHandler::SetMultiplier(0.5f, player);
		if (!NearlyEqual(FallDamageHandler::GetMultiplier(player), 0.5f))
		{
			if (hadOverride)
				FallDamageHandler::SetMultiplier(originalEffective, player);
			else
				FallDamageHandler::ClearMultiplier(player);
			return Fail("override set mismatch");
		}

		FallDamageHandler::ClearMultiplier(player);
		if (!NearlyEqual(FallDamageHandler::GetMultiplier(player), globalValue))
		{
			if (hadOverride)
				FallDamageHandler::SetMultiplier(originalEffective, player);
			return Fail("override clear mismatch");
		}

		if (hadOverride)
			FallDamageHandler::SetMultiplier(originalEffective, player);
		else
			FallDamageHandler::ClearMultiplier(player);

		return Pass();
	}

	static CaseResult Case_NoWeaponSearch(int, PlayerCharacter* player)
	{
		if (!player)
			return Skip("no player");

		bool original = NoWeaponSearch::Get(player);
		NoWeaponSearch::Set(player, true);
		if (!NoWeaponSearch::Get(player))
		{
			NoWeaponSearch::Set(player, original);
			return Fail("enable mismatch");
		}

		NoWeaponSearch::Set(player, false);
		if (NoWeaponSearch::Get(player))
		{
			NoWeaponSearch::Set(player, original);
			return Fail("disable mismatch");
		}

		NoWeaponSearch::Set(player, original);
		return Pass();
	}

	static CaseResult Case_PreventWeaponSwitch(int, PlayerCharacter* player)
	{
		if (!player)
			return Skip("no player");

		bool original = PreventWeaponSwitch::Get(player);
		PreventWeaponSwitch::Set(player, true);
		if (!PreventWeaponSwitch::Get(player))
		{
			PreventWeaponSwitch::Set(player, original);
			return Fail("enable mismatch");
		}

		PreventWeaponSwitch::Set(player, false);
		if (PreventWeaponSwitch::Get(player))
		{
			PreventWeaponSwitch::Set(player, original);
			return Fail("disable mismatch");
		}

		PreventWeaponSwitch::Set(player, original);
		return Pass();
	}

	static CaseResult Case_OnContactWatch(int, PlayerCharacter* player)
	{
		if (!player)
			return Skip("no player");

		bool original = OnContactHandler::IsWatched(player->refID);
		bool originalBase = player->baseForm ? OnContactHandler::IsBaseWatched(player->baseForm->refID) : false;
		OnContactHandler::AddWatch(player->refID);
		if (!OnContactHandler::IsWatched(player->refID))
		{
			if (!original)
				OnContactHandler::RemoveWatch(player->refID);
			return Fail("add mismatch");
		}

		OnContactHandler::RemoveWatch(player->refID);
		if (OnContactHandler::IsWatched(player->refID))
		{
			if (original)
				OnContactHandler::AddWatch(player->refID);
			return Fail("remove mismatch");
		}

		if (original)
			OnContactHandler::AddWatch(player->refID);

		if (!player->baseForm)
			return Pass();

		if (original)
			OnContactHandler::RemoveWatch(player->refID);

		OnContactHandler::AddBaseWatch(player->baseForm->refID);
		if (!OnContactHandler::IsBaseWatched(player->baseForm->refID) ||
			!OnContactHandler::IsRefWatched(player->refID))
		{
			if (!originalBase)
				OnContactHandler::RemoveBaseWatch(player->baseForm->refID);
			if (original)
				OnContactHandler::AddWatch(player->refID);
			return Fail("base add mismatch");
		}

		OnContactHandler::RemoveBaseWatch(player->baseForm->refID);
		if (OnContactHandler::IsBaseWatched(player->baseForm->refID) ||
			OnContactHandler::IsRefWatched(player->refID))
		{
			if (originalBase)
				OnContactHandler::AddBaseWatch(player->baseForm->refID);
			if (original)
				OnContactHandler::AddWatch(player->refID);
			return Fail("base remove mismatch");
		}

		if (originalBase)
			OnContactHandler::AddBaseWatch(player->baseForm->refID);
		if (original)
			OnContactHandler::AddWatch(player->refID);
		return Pass();
	}

	static CaseResult Case_DialogueCamera_InvalidBounds(int, PlayerCharacter*)
	{
		auto state = DialogueCameraHandler::GetDebugState();
		if (DialogueCameraHandler::SetAngleMode(-1))
			return Fail("accepted invalid mode");
		if (DialogueCameraHandler::SetFixedAngle(-1))
			return Fail("accepted invalid fixed angle");
		if (DialogueCameraHandler::GetDebugState().angleMode != state.angleMode)
			return Fail("mode mutated on invalid");
		if (DialogueCameraHandler::GetDebugState().fixedAngle != state.fixedAngle)
			return Fail("angle mutated on invalid");
		return Pass();
	}

	static CaseResult Case_DialogueCamera_StateRoundTrip(int, PlayerCharacter*)
	{
		auto original = DialogueCameraHandler::GetDebugState();

		DialogueCameraHandler::SetEnabled(!original.enabled);
		if (DialogueCameraHandler::IsEnabled() == original.enabled)
		{
			DialogueCameraHandler::RestoreDebugState(original);
			return Fail("enable toggle mismatch");
		}

		if (!DialogueCameraHandler::SetAngleMode(1))
		{
			DialogueCameraHandler::RestoreDebugState(original);
			return Fail("mode set failed");
		}

		if (!DialogueCameraHandler::SetFixedAngle(3))
		{
			DialogueCameraHandler::RestoreDebugState(original);
			return Fail("fixed angle set failed");
		}

		DialogueCameraHandler::SetDolly(1.25f, 80.0f, 0);
		DialogueCameraHandler::SetShakeAmplitude(2.5f);

		auto mutated = DialogueCameraHandler::GetDebugState();
		if (mutated.angleMode != 1 || mutated.fixedAngle != 3)
		{
			DialogueCameraHandler::RestoreDebugState(original);
			return Fail("mode/fixed state mismatch");
		}

		if (!NearlyEqual(mutated.dollySpeed, 1.25f) ||
			!NearlyEqual(mutated.dollyMaxDist, 80.0f) ||
			mutated.dollyRunOnce != 0 ||
			!NearlyEqual(mutated.shakeAmplitude, 2.5f))
		{
			DialogueCameraHandler::RestoreDebugState(original);
			return Fail("dolly/shake mismatch");
		}

		DialogueCameraHandler::RestoreDebugState(original);
		auto restored = DialogueCameraHandler::GetDebugState();
		if (restored.enabled != original.enabled ||
			restored.angleMode != original.angleMode ||
			restored.fixedAngle != original.fixedAngle ||
			restored.currentAngle != original.currentAngle ||
			!NearlyEqual(restored.dollySpeed, original.dollySpeed) ||
			!NearlyEqual(restored.dollyMaxDist, original.dollyMaxDist) ||
			restored.dollyRunOnce != original.dollyRunOnce ||
			!NearlyEqual(restored.shakeAmplitude, original.shakeAmplitude))
			return Fail("restore mismatch");

		return Pass();
	}

	static const CaseDef kCases[] = {
		{ "FallDamage.Global", 0, Case_FallDamage_Global },
		{ "FallDamage.Actor", 0, Case_FallDamage_Actor },
		{ "NoWeaponSearch", 0, Case_NoWeaponSearch },
		{ "PreventWeaponSwitch", 0, Case_PreventWeaponSwitch },
		{ "OnContactWatch", 0, Case_OnContactWatch },
		{ "DialogueCamera.InvalidBounds", 0, Case_DialogueCamera_InvalidBounds },
		{ "DialogueCamera.StateRoundTrip", 0, Case_DialogueCamera_StateRoundTrip },
	};
}

static ParamInfo kParams_RunITRCommandBounds[1] = {
	{ "mode", kParamType_Integer, 1 },
};

DEFINE_COMMAND_PLUGIN(RunITRCommandBounds, "Runs debug-only itr-nvse command bounds smoke tests. Returns failure count.", 0, 1, kParams_RunITRCommandBounds);

static bool Cmd_RunITRCommandBounds_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 mode = 0;
	ExtractArgs(EXTRACT_ARGS, &mode);
	if (mode > 2)
		mode = 0;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	int passed = 0;
	int failed = 0;
	int skipped = 0;

	Log("RunITRCommandBounds: starting mode=%u", mode);
	if (IsConsoleMode())
		Console_Print("RunITRCommandBounds >> starting mode=%u", mode);

	for (const auto& testCase : kCases)
	{
		if ((int)mode < testCase.minMode)
			continue;

		CaseResult caseResult = testCase.fn((int)mode, player);
		ReportCase(testCase.name, caseResult);

		switch (caseResult.status)
		{
			case CaseStatus::kPass:
				passed++;
				break;
			case CaseStatus::kFail:
				failed++;
				break;
			case CaseStatus::kSkip:
				skipped++;
				break;
		}
	}

	Log("RunITRCommandBounds: completed passed=%d failed=%d skipped=%d", passed, failed, skipped);
	if (IsConsoleMode())
		Console_Print("RunITRCommandBounds >> completed passed=%d failed=%d skipped=%d", passed, failed, skipped);

	*result = failed;
	return true;
}

#endif

namespace CommandBoundsCommand {
void RegisterCommands(void* nvsePtr)
{
#ifdef _DEBUG
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_RunITRCommandBounds);
#else
	(void)nvsePtr;
#endif
}
}
