#include "DetectionSoundCommands.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/layout/Process.h"
#include "internal/layout/Combat.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <cmath>
#include <unordered_set>
#include <vector>

extern const _ExtractArgs ExtractArgs;

namespace
{
	constexpr UInt32 kCreateDetectionEventVtableOffset = 0x0FC;
	constexpr UInt32 kProcessLevelHigh = 0;
	constexpr UInt32 kProcessLevelMiddleHigh = 1;

	using CreateDetectionEvent_t = void(__thiscall*)(void*, Actor*, float, float, float, UInt32, UInt32, TESObjectREFR*);
	using SetActionSoundValue_t = void(__thiscall*)(Actor*, UInt32);
	using SetAlert_t = void(__thiscall*)(Actor*, bool);
	using GetAlert_t = bool(__thiscall*)(Actor*);
	using EvaluatePackage_t = void(__thiscall*)(Actor*, bool, bool);
	using AddCombatSearchLocation_t = void(__thiscall*)(void*, void*, float, UInt32);
	using CombatGroupHasSearch_t = bool(__thiscall*)(void*);
	using CombatGroupGetNumTargets_t = UInt32(__thiscall*)(void*);
	using CombatGroupStartSearch_t = void(__thiscall*)(void*);
	using GetTopic_t = TESTopic*(__cdecl*)(UInt32, SInt32);
	using AddPlayerAction_t = void(__thiscall*)(Actor*, UInt32, float, TESForm*);
	using StartGreetingPlayer_t = void(__thiscall*)(Actor*, TESTopic*);

	const auto SetActionSoundValue = reinterpret_cast<SetActionSoundValue_t>(0x8BC240);
	const auto SetAlert = reinterpret_cast<SetAlert_t>(0x8A5E40);
	const auto GetAlert = reinterpret_cast<GetAlert_t>(0x8A5E80);
	const auto EvaluatePackage = reinterpret_cast<EvaluatePackage_t>(0x8A6CE0);
	const auto AddCombatSearchLocation = reinterpret_cast<AddCombatSearchLocation_t>(0x98BFB0);
	const auto CombatGroupHasSearch = reinterpret_cast<CombatGroupHasSearch_t>(0x97EF30);
	const auto CombatGroupGetNumTargets = reinterpret_cast<CombatGroupGetNumTargets_t>(0x5A4320);
	const auto CombatGroupStartSearch = reinterpret_cast<CombatGroupStartSearch_t>(0x98ADD0);
	const auto GetTopic = reinterpret_cast<GetTopic_t>(0x61A2D0);
	const auto AddPlayerAction = reinterpret_cast<AddPlayerAction_t>(0x963EB0);
	const auto StartGreetingPlayer = reinterpret_cast<StartGreetingPlayer_t>(0x8BC3D0);

	constexpr UInt32 kFlag_SameCellOnly = 0x1;
	constexpr UInt32 kFlag_SkipPlayer = 0x2;
	constexpr UInt32 kFlag_NoAlert = 0x4;
	constexpr UInt32 kFlag_IncludePlayer = 0x8;
	constexpr UInt32 kFlag_NoBark = 0x10;
	constexpr UInt32 kDialogueType_Topic = 0;
	constexpr UInt32 kPlayerAction_FireWeapon = 3;
	constexpr UInt32 kTopic_PLAYERFIREWEAPON = 0x000000CB;
	constexpr UInt32 kDefaultAlertTimerMs = 4000;

	struct ForcedAlert
	{
		UInt32 refID;
		DWORD expiresAt;
	};

	struct PendingBark
	{
		UInt32 speakerRefID;
		UInt32 topicRefID;
	};

	std::vector<ForcedAlert> g_forcedAlerts;
	std::vector<PendingBark> g_pendingBarks;

	struct BGSWorldLocationLite
	{
		float x;
		float y;
		float z;
		TESObjectCELL* cellOrWorld;
	};
	static_assert(sizeof(BGSWorldLocationLite) == 0x10, "BGSWorldLocationLite must match BGSWorldLocation");

	bool IsActorRef(TESObjectREFR* ref)
	{
		UInt8 typeID = ref ? ref->typeID : 0;
		return typeID == kFormType_ACHR || typeID == kFormType_ACRE;
	}

	bool IsHearingProcess(BaseProcess* process)
	{
		if (!process)
			return false;

		UInt32 processLevel = process->processLevel;
		return processLevel == kProcessLevelHigh || processLevel == kProcessLevelMiddleHigh;
	}

	bool CreateDetectionEvent(Actor* source, float x, float y, float z, UInt32 soundLevel)
	{
		if (!source)
			return false;

		auto* process = static_cast<BaseProcess*>(Engine::Actor_GetProcess(source));
		if (!process)
			return false;

		auto** vtbl = *reinterpret_cast<void***>(process);
		if (!vtbl)
			return false;

		auto createEvent = reinterpret_cast<CreateDetectionEvent_t>(vtbl[kCreateDetectionEventVtableOffset / sizeof(void*)]);
		if (!createEvent)
			return false;

		createEvent(process, source, x, y, z, soundLevel, 3, nullptr);
		return true;
	}

	float DistanceSquared(TESObjectREFR* ref, float x, float y, float z)
	{
		float dx = ref->posX - x;
		float dy = ref->posY - y;
		float dz = ref->posZ - z;
		return dx * dx + dy * dy + dz * dz;
	}

	float GetSearchStrength(UInt32 soundLevel)
	{
		if (soundLevel == 0)
			return 40.0f;

		float scaled = soundLevel / 20.0f;
		if (scaled > 5.0f)
			scaled = 5.0f;
		return scaled + 55.0f;
	}

	void* GetCombatGroup(Actor* actor)
	{
		if (!actor)
			return nullptr;

		void* controller = Engine::Actor_GetCombatController(actor);
		if (!controller)
			return nullptr;

		return CombatControllerGetCombatGroup(controller);
	}

	bool IsTemporaryAlertTracked(UInt32 refID);
	void TrackTemporaryAlert(Actor* actor);

	void AlertActorForSound(Actor* actor, UInt32 flags)
	{
		Actor* player = *g_thePlayerPtr;
		if (!actor || actor == player || (flags & kFlag_NoAlert))
			return;

		const bool alreadyTracked = IsTemporaryAlertTracked(actor->refID);
		const bool wasAlert = GetAlert(actor);
		SetAlert(actor, true);
		if (!wasAlert || alreadyTracked)
			TrackTemporaryAlert(actor);
		EvaluatePackage(actor, false, false);
	}

	UInt32 GetAlertTimerMs()
	{
		float* seconds = Engine::GetSettingFloatPtr(g_fActorAlertSoundTimerSetting);
		if (!seconds || *seconds <= 0.0f)
			return kDefaultAlertTimerMs;

		return static_cast<UInt32>(*seconds * 1000.0f);
	}

	bool IsTemporaryAlertTracked(UInt32 refID)
	{
		for (const auto& alert : g_forcedAlerts)
		{
			if (alert.refID == refID)
				return true;
		}
		return false;
	}

	void TrackTemporaryAlert(Actor* actor)
	{
		if (!actor)
			return;

		const DWORD expiresAt = GetTickCount() + GetAlertTimerMs();
		for (auto& alert : g_forcedAlerts)
		{
			if (alert.refID == actor->refID)
			{
				alert.expiresAt = expiresAt;
				return;
			}
		}

		g_forcedAlerts.push_back({ actor->refID, expiresAt });
	}

	bool IsExpired(DWORD now, DWORD expiresAt)
	{
		return static_cast<SInt32>(now - expiresAt) >= 0;
	}

	Actor* LookupActor(UInt32 refID)
	{
		auto* ref = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(refID));
		return IsActorRef(ref) ? static_cast<Actor*>(ref) : nullptr;
	}

	bool HasCombatTargets(Actor* actor)
	{
		void* combatGroup = GetCombatGroup(actor);
		return combatGroup && CombatGroupGetNumTargets(combatGroup) > 0;
	}

	bool TryPushSearchLocation(Actor* actor, const BGSWorldLocationLite& location, float strength, std::unordered_set<void*>& seenGroups)
	{
		void* combatGroup = GetCombatGroup(actor);
		if (!combatGroup || !seenGroups.insert(combatGroup).second)
			return false;

		AddCombatSearchLocation(combatGroup, const_cast<BGSWorldLocationLite*>(&location), strength, 0);
		if (!CombatGroupHasSearch(combatGroup) && CombatGroupGetNumTargets(combatGroup) > 0)
			CombatGroupStartSearch(combatGroup);
		return true;
	}

	bool ShouldRequireSameCell(const BGSWorldLocationLite& location, UInt32 flags)
	{
		if (!location.cellOrWorld)
			return false;

		return (flags & kFlag_SameCellOnly) || location.cellOrWorld->IsInterior();
	}

	TESTopic* GetPlayerFireWeaponTopic()
	{
		auto* topic = GetTopic(kDialogueType_Topic, kPlayerAction_FireWeapon);
		if (topic)
			return topic;

		return reinterpret_cast<TESTopic*>(Engine::LookupFormByID(kTopic_PLAYERFIREWEAPON));
	}

	TESTopic* GetResponseTopic(TESTopic* responseTopic, UInt32 flags)
	{
		if (flags & kFlag_NoBark)
			return nullptr;

		if (responseTopic)
			return responseTopic;

		return GetPlayerFireWeaponTopic();
	}

	void QueueResponseBark(Actor* actor, TESTopic* responseTopic)
	{
		if (!actor || !responseTopic)
			return;

		g_pendingBarks.push_back({ actor->refID, responseTopic->refID });
	}

	void StartResponseBark(Actor* actor, TESTopic* topic)
	{
		if (!actor || !topic)
			return;

		Actor* player = *g_thePlayerPtr;
		if (player && topic->refID == kTopic_PLAYERFIREWEAPON)
			AddPlayerAction(player, kPlayerAction_FireWeapon, 2.0f, nullptr);

		StartGreetingPlayer(actor, topic);
	}

	UInt32 ProcessAnonymousSoundForActor(Actor* actor, const BGSWorldLocationLite& location, float radiusSq,
		UInt32 soundLevel, UInt32 flags, TESTopic* responseTopic, std::unordered_set<UInt32>& seenActors,
		std::unordered_set<void*>& seenGroups)
	{
		if (!actor || !seenActors.insert(actor->refID).second)
			return 0;

		Actor* player = *g_thePlayerPtr;
		if (actor == player && ((flags & kFlag_SkipPlayer) || !(flags & kFlag_IncludePlayer)))
			return 0;

		if (!actor->parentCell || !IsHearingProcess(static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor))))
			return 0;

		if (ShouldRequireSameCell(location, flags) && actor->parentCell != location.cellOrWorld)
			return 0;

		if (DistanceSquared(actor, location.x, location.y, location.z) > radiusSq)
			return 0;

		AlertActorForSound(actor, flags);
		if (location.cellOrWorld)
			TryPushSearchLocation(actor, location, GetSearchStrength(soundLevel), seenGroups);
		if (responseTopic)
			QueueResponseBark(actor, responseTopic);
		return 1;
	}

	UInt32 DispatchAnonymousSound(float x, float y, float z, UInt32 soundLevel, float radius, UInt32 flags,
		TESObjectCELL* cell, TESTopic* responseTopic)
	{
		if (radius <= 0.0f)
			return 0;

		auto* processManager = reinterpret_cast<ProcessManagerLite*>(g_processManager);
		if (!processManager || !processManager->objects.data)
			return 0;

		BGSWorldLocationLite location = { x, y, z, cell };
		const float radiusSq = radius * radius;
		responseTopic = GetResponseTopic(responseTopic, flags);

		std::unordered_set<UInt32> seenActors;
		std::unordered_set<void*> seenGroups;
		seenActors.reserve(64);
		seenGroups.reserve(16);

		UInt32 heardCount = 0;
		UInt32 upperBound = processManager->objects.firstFreeEntry;

		for (int bucket = 0; bucket < 2; ++bucket)
		{
			UInt32 begin = processManager->beginOffsets[bucket];
			UInt32 end = processManager->endOffsets[bucket];
			if (begin > upperBound) begin = upperBound;
			if (end > upperBound) end = upperBound;

			auto** current = processManager->objects.data + begin;
			auto** last = processManager->objects.data + end;
			for (; current < last; ++current)
			{
				auto* ref = reinterpret_cast<TESObjectREFR*>(*current);
				if (IsActorRef(ref))
					heardCount += ProcessAnonymousSoundForActor(static_cast<Actor*>(ref), location, radiusSq, soundLevel, flags, responseTopic, seenActors, seenGroups);
			}
		}

		if (*g_thePlayerPtr)
			heardCount += ProcessAnonymousSoundForActor(*g_thePlayerPtr, location, radiusSq, soundLevel, flags, responseTopic, seenActors, seenGroups);

		return heardCount;
	}

	TESObjectCELL* GetDefaultSoundCell(TESObjectREFR* thisObj)
	{
		if (thisObj && thisObj->parentCell)
			return thisObj->parentCell;

		Actor* player = *g_thePlayerPtr;
		return player ? player->parentCell : nullptr;
	}
}

static ParamInfo kParams_CreateDetectionSoundAt[4] =
{
	{ "x", kParamType_Float, 0 },
	{ "y", kParamType_Float, 0 },
	{ "z", kParamType_Float, 0 },
	{ "sound level", kParamType_Integer, 0 },
};

static ParamInfo kParams_CreateAnonymousDetectionSoundAt[7] =
{
	{ "x", kParamType_Float, 0 },
	{ "y", kParamType_Float, 0 },
	{ "z", kParamType_Float, 0 },
	{ "sound level", kParamType_Integer, 0 },
	{ "radius", kParamType_Float, 0 },
	{ "flags", kParamType_Integer, 1 },
	{ "response topic", kParamType_Topic, 1 },
};

DEFINE_COMMAND_PLUGIN(CreateDetectionSoundAt, "Creates an actor-attributed detection sound at world coordinates", 1, 4, kParams_CreateDetectionSoundAt);
DEFINE_COMMAND_PLUGIN(CreateAnonymousDetectionSoundAt, "Creates an anonymous detection sound/search point for loaded actors in radius", 0, 7, kParams_CreateAnonymousDetectionSoundAt);

bool Cmd_CreateDetectionSoundAt_Execute(COMMAND_ARGS)
{
	*result = 0;

	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	UInt32 soundLevel = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &x, &y, &z, &soundLevel) || !IsActorRef(thisObj))
		return true;

	auto* source = static_cast<Actor*>(thisObj);
	SetActionSoundValue(source, soundLevel);
	if (CreateDetectionEvent(source, x, y, z, soundLevel))
		*result = 1;

	return true;
}

bool Cmd_CreateAnonymousDetectionSoundAt_Execute(COMMAND_ARGS)
{
	*result = 0;

	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	UInt32 soundLevel = 0;
	float radius = 0.0f;
	UInt32 flags = 0;
	TESTopic* responseTopic = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &x, &y, &z, &soundLevel, &radius, &flags, &responseTopic))
		return true;

	TESObjectCELL* cell = GetDefaultSoundCell(thisObj);
	*result = DispatchAnonymousSound(x, y, z, soundLevel, radius, flags, cell, responseTopic);
	return true;
}

namespace DetectionSoundCommands
{
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_CreateDetectionSoundAt);
		nvse->RegisterCommand(&kCommandInfo_CreateAnonymousDetectionSoundAt);
	}

	void Update()
	{
		if (!g_forcedAlerts.empty())
		{
			const DWORD now = GetTickCount();
			for (std::size_t i = 0; i < g_forcedAlerts.size();)
			{
				ForcedAlert& alert = g_forcedAlerts[i];
				if (!IsExpired(now, alert.expiresAt))
				{
					++i;
					continue;
				}

				Actor* actor = LookupActor(alert.refID);
				if (actor && HasCombatTargets(actor))
				{
					//still fighting, keep the entry armed so the alert flag gets cleared by us once combat ends
					alert.expiresAt = now + GetAlertTimerMs();
					++i;
					continue;
				}

				if (actor && GetAlert(actor))
				{
					SetAlert(actor, false);
					EvaluatePackage(actor, false, false);
				}

				g_forcedAlerts[i] = g_forcedAlerts.back();
				g_forcedAlerts.pop_back();
			}
		}

		if (!g_pendingBarks.empty())
		{
			std::vector<PendingBark> pendingBarks;
			pendingBarks.swap(g_pendingBarks);

			for (const auto& bark : pendingBarks)
			{
				Actor* actor = LookupActor(bark.speakerRefID);
				auto* topic = reinterpret_cast<TESTopic*>(Engine::LookupFormByID(bark.topicRefID));
				if (actor && topic)
					StartResponseBark(actor, topic);
			}
		}
	}

	void ClearState()
	{
		g_forcedAlerts.clear();
		g_pendingBarks.clear();
	}
}
