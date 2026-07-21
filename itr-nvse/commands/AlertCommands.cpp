#include "AlertCommands.h"
#include "internal/EngineFunctions.h"
#include "internal/layout/Combat.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <vector>

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);

namespace
{
	using SetAlert_t = void(__thiscall*)(Actor*, bool);
	using GetAlert_t = bool(__thiscall*)(Actor*);
	using EvaluatePackage_t = void(__thiscall*)(Actor*, bool, bool);
	using CombatGroupGetNumTargets_t = UInt32(__thiscall*)(void*);

	const auto SetAlert = reinterpret_cast<SetAlert_t>(0x8A5E40);
	const auto GetAlert = reinterpret_cast<GetAlert_t>(0x8A5E80);
	const auto EvaluatePackage = reinterpret_cast<EvaluatePackage_t>(0x8A6CE0);
	const auto CombatGroupGetNumTargets = reinterpret_cast<CombatGroupGetNumTargets_t>(0x5A4320);

	constexpr DWORD kCombatRecheckMs = 2000;

	struct TimedAlert
	{
		UInt32 refID;
		DWORD expiresAt; //0 = until cleared
	};

	std::vector<TimedAlert> g_alerts;
	std::vector<UInt32> g_clearedForSave; //actors whose alert bit we dropped for an in-progress save
	bool g_restorePending = false;

	bool IsActorRef(TESObjectREFR* ref)
	{
		UInt8 typeID = ref ? ref->typeID : 0;
		return typeID == kFormType_ACHR || typeID == kFormType_ACRE;
	}

	Actor* LookupActor(UInt32 refID)
	{
		auto* ref = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(refID));
		return IsActorRef(ref) ? static_cast<Actor*>(ref) : nullptr;
	}

	TimedAlert* FindAlert(UInt32 refID)
	{
		for (auto& alert : g_alerts)
		{
			if (alert.refID == refID)
				return &alert;
		}
		return nullptr;
	}

	void RemoveAlert(UInt32 refID)
	{
		for (std::size_t i = 0; i < g_alerts.size(); ++i)
		{
			if (g_alerts[i].refID == refID)
			{
				g_alerts[i] = g_alerts.back();
				g_alerts.pop_back();
				return;
			}
		}
	}

	bool HasCombatTargets(Actor* actor)
	{
		void* controller = Engine::Actor_GetCombatController(actor);
		void* group = controller ? CombatControllerGetCombatGroup(controller) : nullptr;
		return group && CombatGroupGetNumTargets(group) > 0;
	}

	bool IsExpired(DWORD now, DWORD expiresAt)
	{
		return static_cast<SInt32>(now - expiresAt) >= 0;
	}
}

static ParamInfo kParams_SetAlertNS[2] =
{
	{ "toggle", kParamType_Integer, 0 },
	{ "duration", kParamType_Float, 1 },
};

DEFINE_COMMAND_PLUGIN(SetAlertNS, "Sets the actor's alert posture without persisting it across a save, optionally auto-clearing after duration seconds", 1, 2, kParams_SetAlertNS);
DEFINE_COMMAND_PLUGIN(GetAlertNS, "Returns remaining SetAlertNS seconds, -1 if timerless, 0 if untracked", 1, 0, nullptr);

bool Cmd_SetAlertNS_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 toggle = 0;
	float duration = 0.0f;
	if (!ExtractArgs(EXTRACT_ARGS, &toggle, &duration) || !IsActorRef(thisObj))
		return true;

	auto* actor = static_cast<Actor*>(thisObj);
	if (toggle)
	{
		SetAlert(actor, true);
		EvaluatePackage(actor, false, false);

		DWORD expiresAt = 0;
		if (duration > 0.0f)
		{
			if (duration > 86400.0f) //clamp to a day, ms conversion overflow guard
				duration = 86400.0f;
			expiresAt = GetTickCount() + static_cast<DWORD>(duration * 1000.0f);
			if (!expiresAt)
				expiresAt = 1;
		}

		if (TimedAlert* existing = FindAlert(actor->refID))
			existing->expiresAt = expiresAt;
		else
			g_alerts.push_back({ actor->refID, expiresAt });
		Log("SetAlertNS: %08X on, duration %.2f", actor->refID, duration);
	}
	else
	{
		RemoveAlert(actor->refID);
		if (GetAlert(actor))
		{
			SetAlert(actor, false);
			EvaluatePackage(actor, false, false);
		}
		Log("SetAlertNS: %08X off", actor->refID);
	}

	*result = 1;
	return true;
}

bool Cmd_GetAlertNS_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!IsActorRef(thisObj))
		return true;

	if (const TimedAlert* alert = FindAlert(thisObj->refID))
	{
		if (!alert->expiresAt)
			*result = -1;
		else
		{
			const SInt32 remainingMs = static_cast<SInt32>(alert->expiresAt - GetTickCount());
			if (remainingMs > 0)
				*result = remainingMs / 1000.0;
		}
	}

	return true;
}

namespace AlertCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_SetAlertNS);
		nvse->RegisterCommand(&kCommandInfo_GetAlertNS);
	}

	void OnSaveGame()
	{
		//drop the real alert bit for tracked actors so no serialisation path writes it,
		//then restore next frame. SetAlert is a pure process+0x30 bit-8 flip with no package
		//eval, and the save freezes the sim, so the actor never animates the holster
		g_clearedForSave.clear();
		for (const auto& alert : g_alerts)
		{
			Actor* actor = LookupActor(alert.refID);
			if (actor && GetAlert(actor))
			{
				SetAlert(actor, false);
				g_clearedForSave.push_back(alert.refID);
			}
		}
		g_restorePending = !g_clearedForSave.empty();
	}

	void Update()
	{
		if (g_restorePending)
		{
			for (UInt32 refID : g_clearedForSave)
			{
				Actor* actor = LookupActor(refID);
				if (actor)
					SetAlert(actor, true);
			}
			g_clearedForSave.clear();
			g_restorePending = false;
		}

		if (g_alerts.empty())
			return;

		const DWORD now = GetTickCount();
		for (std::size_t i = 0; i < g_alerts.size();)
		{
			TimedAlert& alert = g_alerts[i];
			if (!alert.expiresAt || !IsExpired(now, alert.expiresAt))
			{
				++i;
				continue;
			}

			Actor* actor = LookupActor(alert.refID);
			if (actor && HasCombatTargets(actor))
			{
				//still fighting, defer expiry so the posture clears once combat ends
				alert.expiresAt = now + kCombatRecheckMs;
				++i;
				continue;
			}

			if (actor && GetAlert(actor))
			{
				SetAlert(actor, false);
				EvaluatePackage(actor, false, false);
			}
			Log("SetAlertNS: %08X expired", alert.refID);

			g_alerts[i] = g_alerts.back();
			g_alerts.pop_back();
		}
	}

	void ClearState()
	{
		g_alerts.clear();
		g_clearedForSave.clear();
		g_restorePending = false;
	}
}
