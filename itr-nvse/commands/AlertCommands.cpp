#include "AlertCommands.h"
#include "internal/EngineFunctions.h"
#include "internal/layout/Combat.h"
#include "internal/Detours.h"
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

	constexpr UInt8 kAlertBit = 0x08;      //process+0x30 Alert bit
	constexpr UInt32 kBakeSite = 0x90FB1B; //LowProcess::SaveGame writes process+0x30 here
	constexpr UInt32 kWriter = 0x8579B0;   //save-buffer byte writer, __thiscall(buf, ptr, len)

	Detours::CallDetour s_bakeMaskDetour;
	using WriteSaveBytes_t = int(__thiscall*)(void*, void*, int);
	UInt8 g_maskedFlag = 0;

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

	bool ProcessIsTracked(void* process)
	{
		for (const auto& alert : g_alerts)
		{
			Actor* actor = LookupActor(alert.refID);
			//actor+0x68 = process pointer (SetAlert 0x8A5E40 uses this[26])
			if (actor && *reinterpret_cast<void**>(reinterpret_cast<UInt8*>(actor) + 0x68) == process)
				return true;
		}
		return false;
	}

	//mask the Alert bit out of the serialised process flag byte for plugin-tracked
	//actors so SetAlertNS never bakes, without touching live state
	int __fastcall Hook_SerialiseProcessFlags(void* saveBuf, void*, UInt8* flagByte, int len)
	{
		auto original = reinterpret_cast<WriteSaveBytes_t>(s_bakeMaskDetour.GetOverwrittenAddr());
		if (!g_alerts.empty() && flagByte && (*flagByte & kAlertBit)
			&& ProcessIsTracked(reinterpret_cast<UInt8*>(flagByte) - 0x30))
		{
			g_maskedFlag = *flagByte & ~kAlertBit;
			return original(saveBuf, &g_maskedFlag, len);
		}
		return original(saveBuf, flagByte, len);
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

	void Init()
	{
		//mask the Alert bit at the save-bake site instead of stripping the live bit on
		//kMessage_SaveGame, which xNVSE dispatches after the .fos is already written
		if (!s_bakeMaskDetour.WriteRelCallIfTarget(kBakeSite, kWriter, Hook_SerialiseProcessFlags))
			Log("AlertCommands: save-bake site %08X not vanilla; SetAlertNS may persist across saves", kBakeSite);
	}

	void Update()
	{
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
	}
}
