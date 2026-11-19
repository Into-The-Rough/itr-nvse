#include "DetectionSoundCommands.h"
#include "internal/DetectionSoundLogic.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/RayCast.h"
#include "internal/globals.h"
#include "internal/layout/Process.h"
#include "internal/layout/Combat.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <cmath>
#include <unordered_set>
#include <vector>

extern const _ExtractArgs ExtractArgs;

namespace {
	constexpr UInt32 kCreateDetectionEventVtableOffset = 0x0FC;
	constexpr UInt32 kGetSitSleepStateVtableOffset = 0x214;
	constexpr UInt32 kIsSpeakingVtableOffset = 0x30C;
	constexpr UInt32 kProcessLevelHigh = 0;
	constexpr UInt32 kProcessLevelMiddleHigh = 1;
	constexpr SInt32 kHeadTrackType_Script = 2;
	constexpr UInt32 kProcedureType_Search = 10;
	constexpr UInt32 kProcedureGetTypeVtableOffset = 0x34;
	constexpr UInt8 kLineOfSightLayer = 6;
	constexpr DWORD kThrownSettleDelayMs = 700;
	constexpr float kGroundSnapDrop = 512.0f;

	using CreateDetectionEvent_t = void(__thiscall*)(void*, Actor*, float, float, float, UInt32, UInt32, TESObjectREFR*);
	using SetActionSoundValue_t = void(__thiscall*)(Actor*, UInt32);
	using SetAlert_t = void(__thiscall*)(Actor*, bool);
	using GetAlert_t = bool(__thiscall*)(Actor*);
	using EvaluatePackage_t = void(__thiscall*)(Actor*, bool, bool);
	using AddCombatSearchLocation_t = void(__thiscall*)(void*, void*, float, UInt32);
	using CombatGroupHasSearch_t = bool(__thiscall*)(void*);
	using CombatGroupGetNumTargets_t = UInt32(__thiscall*)(void*);
	using CombatGroupStartSearch_t = void(__thiscall*)(void*);
	using CombatGroupFindTarget_t = CombatTargetView*(__thiscall*)(void*, Actor*);
	using GetProcedureByType_t = void*(__thiscall*)(void*, UInt32);
	using AITimerExpire_t = float*(__thiscall*)(float*);
	using GetTopic_t = TESTopic*(__cdecl*)(UInt32, SInt32);
	using AddPlayerAction_t = void(__thiscall*)(Actor*, UInt32, float, TESForm*);
	using StartGreetingPlayer_t = void(__thiscall*)(Actor*, TESTopic*);
	using SetDialogueTarget_t = void(__thiscall*)(Actor*, Actor*);
	using ProcessGreet_t = void(__thiscall*)(void*, Actor*, TESTopic*, bool, bool, bool, bool);
	using StartCombat_t = void(__thiscall*)(Actor*, Actor*, void*, char, char, char, UInt32, char, void*);
	using GetShouldAttackActor_t = bool(__thiscall*)(Actor*, Actor*, bool, int*, bool);
	using ActorMoverSetLookAt_t = void(__thiscall*)(void*, float, float, float);
	using ActorMoverClearLookAt_t = void(__thiscall*)(void*);
	using HighProcessSetHeadTrack_t = void(__thiscall*)(void*, TESObjectREFR*);
	using HighProcessClearDefaultHeadTrack_t = void(__thiscall*)(void*);
	using HighProcessClearScriptHeadTrack_t = void(__thiscall*)(void*, bool);
	using HighProcessGetHeadTrackTarget_t = TESObjectREFR*(__thiscall*)(void*, SInt32);
	using FormFlagQuery_t = bool(__thiscall*)(TESForm*);

	const auto SetActionSoundValue = reinterpret_cast<SetActionSoundValue_t>(0x8BC240);
	const auto SetAlert = reinterpret_cast<SetAlert_t>(0x8A5E40);
	const auto GetAlert = reinterpret_cast<GetAlert_t>(0x8A5E80);
	const auto EvaluatePackage = reinterpret_cast<EvaluatePackage_t>(0x8A6CE0);
	const auto AddCombatSearchLocation = reinterpret_cast<AddCombatSearchLocation_t>(0x98BFB0);
	const auto CombatGroupHasSearch = reinterpret_cast<CombatGroupHasSearch_t>(0x97EF30);
	const auto CombatGroupGetNumTargets = reinterpret_cast<CombatGroupGetNumTargets_t>(0x5A4320);
	const auto CombatGroupStartSearch = reinterpret_cast<CombatGroupStartSearch_t>(0x98ADD0);
	const auto CombatGroup_FindTarget = reinterpret_cast<CombatGroupFindTarget_t>(0x9865D0);
	const auto CombatController_GetProcedureByType = reinterpret_cast<GetProcedureByType_t>(0x980400);
	const auto AITimer_Expire = reinterpret_cast<AITimerExpire_t>(0x97F220);
	const auto GetTopic = reinterpret_cast<GetTopic_t>(0x61A2D0);
	const auto AddPlayerAction = reinterpret_cast<AddPlayerAction_t>(0x963EB0);
	const auto StartGreetingPlayer = reinterpret_cast<StartGreetingPlayer_t>(0x8BC3D0);
	const auto Actor_SetDialogueTarget = reinterpret_cast<SetDialogueTarget_t>(0x57BD60);
	const auto HighProcess_ProcessGreet = reinterpret_cast<ProcessGreet_t>(0x8DBE30);
	const auto Actor_StartCombat = reinterpret_cast<StartCombat_t>(0x89FCF0);
	const auto Actor_GetShouldAttackActor = reinterpret_cast<GetShouldAttackActor_t>(0x8B06D0);
	const auto ActorMover_SetLookAtPosition = reinterpret_cast<ActorMoverSetLookAt_t>(0x9DE160);
	const auto ActorMover_ClearLookAtPosition = reinterpret_cast<ActorMoverClearLookAt_t>(0x9DE230);
	const auto HighProcess_SetScriptHeadTrackTarget = reinterpret_cast<HighProcessSetHeadTrack_t>(0x900F50);
	const auto HighProcess_SetDialogHeadTrackTarget = reinterpret_cast<HighProcessSetHeadTrack_t>(0x901050);
	const auto HighProcess_ClearDefaultHeadTrackTarget = reinterpret_cast<HighProcessClearDefaultHeadTrack_t>(0x9010F0);
	const auto HighProcess_ClearScriptHeadTrackTarget = reinterpret_cast<HighProcessClearScriptHeadTrack_t>(0x901190);
	const auto HighProcess_GetCurrentHeadTrackTarget = reinterpret_cast<HighProcessGetHeadTrackTarget_t>(0x901640);
	const auto TESForm_GetDeleted = reinterpret_cast<FormFlagQuery_t>(0x440D80);
	const auto TESForm_GetDisabled = reinterpret_cast<FormFlagQuery_t>(0x440DA0);

	constexpr UInt32 kFlag_SameCellOnly = 0x1;
	constexpr UInt32 kFlag_SkipPlayer = 0x2;
	constexpr UInt32 kFlag_NoAlert = 0x4;
	constexpr UInt32 kFlag_IncludePlayer = 0x8;
	constexpr UInt32 kFlag_NoBark = 0x10;
	constexpr UInt32 kFlag_FaceOrigin = 0x20;
	constexpr UInt32 kFlag_StartCautionSearch = 0x40;
	constexpr UInt32 kDialogueType_Topic = 0;
	constexpr UInt32 kDialogueType_Detection = 4;
	constexpr SInt32 kDetectionSubtype_AlertIdle = 0;
	constexpr SInt32 kDetectionSubtype_NormalToAlert = 2;
	constexpr UInt32 kPlayerAction_FireWeapon = 3;
	constexpr UInt32 kTopic_PLAYERFIREWEAPON = 0x000000CB;
	constexpr UInt32 kDefaultAlertTimerMs = 4000;

	struct ForcedAlert {
		UInt32 refID;
		DWORD expiresAt;
		float originX;
		float originY;
		float originZ;
		UInt32 headTrackRefID;
		bool clearAlertOnExpire;
		bool faceOrigin;
	};

	struct PendingBark {
		UInt32 speakerRefID;
		UInt32 topicRefID;
		bool useProcessGreet;
	};

	struct PendingSearchPush {
		UInt32 actorRefID;
		UInt32 sourceRefID;
		float x;
		float y;
		float z;
		UInt32 spaceRefID;
		float strength;
		DWORD settleAtMs;
		UInt8 framesLeft;
		bool awaitingSettle;
	};

	std::vector<ForcedAlert> g_forcedAlerts;
	std::vector<PendingBark> g_pendingBarks;
	std::vector<PendingSearchPush> g_pendingSearchPushes;
	std::vector<DetectionSoundLogic::ActorCooldown> g_reactionCooldowns;
	std::vector<DetectionSoundLogic::ActorCooldown> g_investigatorCooldowns;
	DWORD g_lastBarkTick = 0;
	bool g_hasLastBark = false;

	bool IsActorRef(TESObjectREFR* ref)
	{
		UInt8 typeID = ref ? ref->typeID : 0;
		return typeID == kFormType_ACHR || typeID == kFormType_ACRE;
	}

	TESObjectREFR* LookupRef(UInt32 refID)
	{
		if (!refID)
			return nullptr;
		auto* form = static_cast<TESForm*>(Engine::LookupFormByID(refID));
		return (form && form->IsReference()) ? static_cast<TESObjectREFR*>(form) : nullptr;
	}

	Actor* LookupActor(UInt32 refID)
	{
		auto* ref = reinterpret_cast<TESObjectREFR*>(Engine::LookupFormByID(refID));
		return IsActorRef(ref) ? static_cast<Actor*>(ref) : nullptr;
	}

	TESForm* LookupWorldSpaceOrCell(UInt32 refID)
	{
		if (!refID)
			return nullptr;
		auto* form = static_cast<TESForm*>(Engine::LookupFormByID(refID));
		if (!form)
			return nullptr;
		return (form->typeID == kFormType_WorldSpace || form->typeID == kFormType_Cell) ? form : nullptr;
	}

	//0x575CA0 stores TESWorldSpace* in exteriors and TESObjectCELL* only in interiors
	TESForm* GetWorldLocationSpace(TESObjectCELL* cell)
	{
		if (!cell)
			return nullptr;
		return cell->worldSpace ? static_cast<TESForm*>(cell->worldSpace) : static_cast<TESForm*>(cell);
	}

	float SnapSearchZToGround(float x, float y, float z)
	{
		RayCastData ray = {};
		const float startZ = z + 8.0f;
		const float endZ = z - kGroundSnapDrop;
		ray.pos0[0] = x * kHavokScale;
		ray.pos0[1] = y * kHavokScale;
		ray.pos0[2] = startZ * kHavokScale;
		ray.pos1[0] = x * kHavokScale;
		ray.pos1[1] = y * kHavokScale;
		ray.pos1[2] = endZ * kHavokScale;
		ray.hitFraction = 1.0f;
		ray.unk44[0] = 0xFFFFFFFF;
		ray.unk44[6] = 0xFFFFFFFF;
		ray.layerType = kLineOfSightLayer;
		if (Engine::TESPickObject(&ray, true) && !ray.budgetSpent && ray.cdBody && ray.hitFraction > 0.0f && ray.hitFraction < 1.0f)
			return startZ - (startZ - endZ) * ray.hitFraction + 6.0f;
		return z;
	}

	int GetSitSleepState(Actor* actor)
	{
		if (!actor)
			return 0;
		void** vtbl = *reinterpret_cast<void***>(actor);
		return reinterpret_cast<int(__thiscall*)(Actor*)>(vtbl[kGetSitSleepStateVtableOffset / sizeof(void*)])(actor);
	}

	//1..5 = sitting chain, 6..10 = bed/sleep chain
	bool IsSleepingState(int sitSleepState)
	{
		return sitSleepState >= 6;
	}

	bool IsProcessSpeaking(BaseProcess* process)
	{
		if (!process)
			return false;
		void** vtbl = *reinterpret_cast<void***>(process);
		return reinterpret_cast<bool(__thiscall*)(BaseProcess*)>(vtbl[kIsSpeakingVtableOffset / sizeof(void*)])(process);
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

	bool HasCombatTargets(Actor* actor);
	ForcedAlert* FindTrackedAlert(UInt32 refID);
	void TrackTemporaryAlert(Actor* actor, bool clearAlert, bool faceOrigin, const BGSWorldLocationView& location, UInt32 headTrackRefID);

	UInt32 GetAlertTimerMs()
	{
		float* seconds = Engine::GetSettingFloatPtr(g_fActorAlertSoundTimerSetting);
		if (!seconds || *seconds <= 0.0f)
			return kDefaultAlertTimerMs;

		return static_cast<UInt32>(*seconds * 1000.0f);
	}

	ForcedAlert* FindTrackedAlert(UInt32 refID)
	{
		for (auto& alert : g_forcedAlerts) {
			if (alert.refID == refID)
				return &alert;
		}
		return nullptr;
	}

	void ClearScriptHeadTrackIfOwned(Actor* actor, UInt32 headTrackRefID)
	{
		if (!actor || !headTrackRefID)
			return;
		auto* process = static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor));
		if (!process || process->processLevel != kProcessLevelHigh)
			return;
		TESObjectREFR* tracked = LookupRef(headTrackRefID);
		if (!tracked || HighProcess_GetCurrentHeadTrackTarget(process, kHeadTrackType_Script) == tracked)
			HighProcess_ClearScriptHeadTrackTarget(process, false);
	}

	void ClearOriginFacing(Actor* actor, ForcedAlert& alert)
	{
		if (!alert.faceOrigin)
			return;
		if (actor && actor->actorMover)
			ActorMover_ClearLookAtPosition(actor->actorMover);
		ClearScriptHeadTrackIfOwned(actor, alert.headTrackRefID);
		alert.faceOrigin = false;
		alert.headTrackRefID = 0;
	}

	void TrackTemporaryAlert(Actor* actor, bool clearAlert, bool faceOrigin, const BGSWorldLocationView& location, UInt32 headTrackRefID)
	{
		if (!actor)
			return;

		const DWORD expiresAt = GetTickCount() + GetAlertTimerMs();
		if (ForcedAlert* existing = FindTrackedAlert(actor->refID)) {
			existing->expiresAt = expiresAt;
			if (clearAlert)
				existing->clearAlertOnExpire = true;
			if (faceOrigin) {
				if (existing->headTrackRefID && existing->headTrackRefID != headTrackRefID)
					ClearScriptHeadTrackIfOwned(actor, existing->headTrackRefID);
				existing->originX = location.x;
				existing->originY = location.y;
				existing->originZ = location.z;
				existing->headTrackRefID = headTrackRefID;
				existing->faceOrigin = true;
			}
			return;
		}

		g_forcedAlerts.push_back({
			actor->refID,
			expiresAt,
			location.x,
			location.y,
			location.z,
			faceOrigin ? headTrackRefID : 0,
			clearAlert,
			faceOrigin,
		});
	}

	bool ApplyOriginFacing(Actor* actor, const BGSWorldLocationView& location, UInt32 sourceRefID, UInt32* outHeadTrackRefID)
	{
		if (outHeadTrackRefID)
			*outHeadTrackRefID = 0;
		Actor* player = *g_thePlayerPtr;
		if (!actor || actor == player || HasCombatTargets(actor))
			return false;
		const int sitSleepState = GetSitSleepState(actor);
		if (IsSleepingState(sitSleepState))
			return false;

		bool applied = false;
		if (sitSleepState == 0 && actor->actorMover) {
			ActorMover_SetLookAtPosition(actor->actorMover, location.x, location.y, location.z);
			applied = true;
		}

		auto* process = static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor));
		if (process && process->processLevel == kProcessLevelHigh) {
			TESObjectREFR* sourceRef = LookupRef(sourceRefID);
			if (sourceRef && sourceRef != player && !TESForm_GetDeleted(sourceRef) && !TESForm_GetDisabled(sourceRef)) {
				HighProcess_SetScriptHeadTrackTarget(process, sourceRef);
				if (outHeadTrackRefID)
					*outHeadTrackRefID = sourceRef->refID;
				applied = true;
			} else if (sitSleepState == 0) {
				HighProcess_ClearDefaultHeadTrackTarget(process);
			}
		}
		return applied;
	}

	void AlertActorForSound(Actor* actor, UInt32 flags, const BGSWorldLocationView& location, UInt32 sourceRefID)
	{
		Actor* player = *g_thePlayerPtr;
		if (!actor || actor == player)
			return;

		const UInt32 actorRefID = actor->refID;
		bool clearAlert = false;
		if (!(flags & kFlag_NoAlert) && GetSitSleepState(actor) == 0) {
			const ForcedAlert* existing = FindTrackedAlert(actorRefID);
			const bool hadClearAlert = existing && existing->clearAlertOnExpire;
			const bool wasAlert = GetAlert(actor);
			SetAlert(actor, true);
			clearAlert = !wasAlert || hadClearAlert;
			EvaluatePackage(actor, false, false);
			actor = LookupActor(actorRefID);
			if (!actor)
				return;
		}

		UInt32 headTrackRefID = 0;
		bool facedOrigin = false;
		if (flags & kFlag_FaceOrigin)
			facedOrigin = ApplyOriginFacing(actor, location, sourceRefID, &headTrackRefID);

		if (clearAlert || facedOrigin)
			TrackTemporaryAlert(actor, clearAlert, facedOrigin, location, headTrackRefID);
	}

	bool IsExpired(DWORD now, DWORD expiresAt)
	{
		return static_cast<SInt32>(now - expiresAt) >= 0;
	}

	bool HasCombatTargets(Actor* actor)
	{
		void* combatGroup = GetCombatGroup(actor);
		return combatGroup && CombatGroupGetNumTargets(combatGroup) > 0;
	}

	bool IsSearchProcedure(void* proc)
	{
		if (!proc)
			return false;
		void** vtbl = *reinterpret_cast<void***>(proc);
		return vtbl && reinterpret_cast<UInt32(__thiscall*)(void*)>(vtbl[kProcedureGetTypeVtableOffset / sizeof(void*)])(proc) == kProcedureType_Search;
	}

	void RestartSearchProcedure(Actor* actor)
	{
		void* controller = actor ? Engine::Actor_GetCombatController(actor) : nullptr;
		if (!controller)
			return;
		auto* view = reinterpret_cast<CombatControllerView*>(controller);
		if (IsSearchProcedure(view->movementProcedure))
			CombatProcedureSetStatus(view->movementProcedure, 2);
		if (IsSearchProcedure(view->attackProcedure))
			CombatProcedureSetStatus(view->attackProcedure, 2);
		if (void* proc = CombatController_GetProcedureByType(controller, kProcedureType_Search))
			CombatProcedureSetStatus(proc, 2);
	}

	//0x985410 seeds CombatTarget::lastSeenLocation with player coords on StartCombat; overwrite with sound origin while undetected
	void SetUndetectedTargetSearchOrigin(void* combatGroup, Actor* player, const BGSWorldLocationView& location)
	{
		if (!combatGroup || !player || !location.form)
			return;
		CombatTargetView* target = CombatGroup_FindTarget(combatGroup, player);
		if (target && target->detectionLevel <= 0)
			target->lastSeenLocation = location;
	}

	void ApplyScoutSearchLocation(Actor* scout, void* combatGroup, Actor* player, const BGSWorldLocationView& location, float strength, bool replaceExisting)
	{
		if (!combatGroup || !location.form)
			return;

		SetUndetectedTargetSearchOrigin(combatGroup, player, location);
		if (replaceExisting)
			CombatGroupMarkSearchLocationsInvestigated(combatGroup);

		AddCombatSearchLocation(combatGroup, const_cast<BGSWorldLocationView*>(&location), strength, 0);
		if (!CombatGroupHasSearch(combatGroup) && CombatGroupGetNumTargets(combatGroup) > 0) {
			CombatGroupStartSearch(combatGroup);
		} else if (CombatGroupHasSearch(combatGroup)) {
			if (float* updateTimer = CombatGroupGetSearchUpdateTimer(combatGroup))
				AITimer_Expire(updateTimer);
			if (float* areaTimer = CombatGroupGetSearchAreaTimer(combatGroup))
				AITimer_Expire(areaTimer);
			if (replaceExisting && scout)
				RestartSearchProcedure(scout);
		}
	}

	void QueuePendingSearchPush(UInt32 actorRefID, UInt32 sourceRefID, const BGSWorldLocationView& location, float strength)
	{
		if (!actorRefID || !location.form)
			return;
		const UInt32 spaceRefID = location.form->refID;
		const DWORD settleAtMs = sourceRefID ? (GetTickCount() + kThrownSettleDelayMs) : 0;
		const PendingSearchPush next = {
			actorRefID, sourceRefID, location.x, location.y, location.z, spaceRefID, strength, settleAtMs, 2, sourceRefID != 0
		};
		for (auto& entry : g_pendingSearchPushes) {
			if (entry.actorRefID == actorRefID) {
				entry = next;
				return;
			}
		}
		g_pendingSearchPushes.push_back(next);
	}

	bool TryPushSearchLocation(Actor* actor, const BGSWorldLocationView& location, float strength, std::unordered_set<void*>& seenGroups)
	{
		void* combatGroup = GetCombatGroup(actor);
		if (!combatGroup || !seenGroups.insert(combatGroup).second)
			return false;

		AddCombatSearchLocation(combatGroup, const_cast<BGSWorldLocationView*>(&location), strength, 0);
		if (!CombatGroupHasSearch(combatGroup) && CombatGroupGetNumTargets(combatGroup) > 0)
			CombatGroupStartSearch(combatGroup);
		return true;
	}

	bool TryStartUndetectedCautionSearch(UInt32 actorRefID, UInt32 sourceRefID, const BGSWorldLocationView& location, float strength, std::unordered_set<void*>& seenGroups)
	{
		Actor* actor = LookupActor(actorRefID);
		Actor* player = *g_thePlayerPtr;
		if (!actor || !player || actor == player || !location.form)
			return false;
		if (Engine::Actor_GetCombatController(actor) || IsSleepingState(GetSitSleepState(actor)))
			return false;

		auto* process = static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor));
		if (!process || process->processLevel != kProcessLevelHigh)
			return false;
		if (Engine::Actor_GetDetectionValue(actor, player) > 0)
			return false;

		int reaction = 0;
		if (!Actor_GetShouldAttackActor(actor, player, false, &reaction, false))
			return false;

		//detectionLevel 0 enters [CAUTION] without revealing player coordinates or recording a crime
		Actor_StartCombat(actor, player, nullptr, 0, 0, 1, 0, 1, nullptr);

		actor = LookupActor(actorRefID);
		player = *g_thePlayerPtr;
		if (!actor || !player)
			return false;
		void* combatGroup = GetCombatGroup(actor);
		if (!combatGroup)
			return false;

		seenGroups.insert(combatGroup);
		ApplyScoutSearchLocation(actor, combatGroup, player, location, strength, true);
		QueuePendingSearchPush(actorRefID, sourceRefID, location, strength);
		return true;
	}

	bool TryRedirectCautionScout(UInt32 actorRefID, UInt32 sourceRefID, const BGSWorldLocationView& location, float strength, std::unordered_set<void*>& seenGroups)
	{
		Actor* actor = LookupActor(actorRefID);
		Actor* player = *g_thePlayerPtr;
		if (!actor || !player || !location.form)
			return false;
		if (Engine::Actor_GetDetectionValue(actor, player) > 0)
			return false;
		void* combatGroup = GetCombatGroup(actor);
		if (!combatGroup)
			return false;

		seenGroups.insert(combatGroup);
		ApplyScoutSearchLocation(actor, combatGroup, player, location, strength, true);
		QueuePendingSearchPush(actorRefID, sourceRefID, location, strength);
		return true;
	}

	bool IsSameSoundSpace(TESObjectCELL* actorCell, TESObjectCELL* soundCell, UInt32 flags)
	{
		if (!actorCell)
			return false;
		if (!soundCell)
			return true;
		if ((flags & kFlag_SameCellOnly) || soundCell->IsInterior() || actorCell->IsInterior())
			return actorCell == soundCell;
		return !soundCell->worldSpace || actorCell->worldSpace == soundCell->worldSpace;
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

	void QueueResponseBark(Actor* actor, TESTopic* responseTopic, bool useProcessGreet)
	{
		if (!actor || !responseTopic)
			return;

		g_pendingBarks.push_back({actor->refID, responseTopic->refID, useProcessGreet});
	}

	void StartResponseBark(Actor* actor, TESTopic* topic, bool useProcessGreet)
	{
		if (!actor || !topic || Engine::Actor_IsDead(actor, false))
			return;

		const UInt32 speakerRefID = actor->refID;
		Actor* player = *g_thePlayerPtr;
		if (useProcessGreet) {
			auto* process = static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor));
			if (!process || process->processLevel != kProcessLevelHigh || IsProcessSpeaking(process))
				return;

			UInt32* lipDistance = GetVoiceLipDistanceLimit();
			const UInt32 oldLipDistance = *lipDistance;
			*lipDistance = 0x7FFFFFFF;

			//0x5C8A50 (Say) copies Script headtrack to Dialog headtrack and clears player dialogue target
			TESObjectREFR* scriptHeadTrack = HighProcess_GetCurrentHeadTrackTarget(process, kHeadTrackType_Script);
			Actor_SetDialogueTarget(actor, nullptr);
			HighProcessSetDialogueTarget(process, nullptr);
			HighProcess_SetDialogHeadTrackTarget(process, scriptHeadTrack);

			TESTopic* usedTopic = topic;
			HighProcess_ProcessGreet(process, actor, usedTopic, false, false, false, false);
			actor = LookupActor(speakerRefID);
			process = actor ? static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor)) : nullptr;
			void* item = (process && process->processLevel == kProcessLevelHigh) ? HighProcessGetCurrentDialogueItem(process) : nullptr;
			if (!item && actor && process && process->processLevel == kProcessLevelHigh) {
				TESTopic* alertIdle = GetTopic(kDialogueType_Detection, kDetectionSubtype_AlertIdle);
				TESTopic* normalToAlert = GetTopic(kDialogueType_Detection, kDetectionSubtype_NormalToAlert);
				TESTopic* fallback = nullptr;
				if (topic == alertIdle)
					fallback = normalToAlert;
				else if (topic == normalToAlert)
					fallback = alertIdle;
				if (fallback) {
					usedTopic = fallback;
					HighProcess_ProcessGreet(process, actor, usedTopic, false, false, false, false);
					actor = LookupActor(speakerRefID);
					process = actor ? static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor)) : nullptr;
					item = (process && process->processLevel == kProcessLevelHigh) ? HighProcessGetCurrentDialogueItem(process) : nullptr;
				}
			}

			*lipDistance = oldLipDistance;
			if (item) {
				g_lastBarkTick = GetTickCount();
				g_hasLastBark = true;
			}
			Log("DetectionSound: bark speaker=%08X topic=%08X item=%p", speakerRefID, usedTopic ? usedTopic->refID : 0, item);
			return;
		}

		if (player && topic->refID == kTopic_PLAYERFIREWEAPON)
			AddPlayerAction(player, kPlayerAction_FireWeapon, 2.0f, nullptr);

		StartGreetingPlayer(actor, topic);
	}

	struct ListenerCandidate {
		UInt32 refID;
		float distSq;
	};

	void CollectListenerCandidate(Actor* actor, TESObjectCELL* soundCell, const BGSWorldLocationView& location, float radiusSq,
		UInt32 flags, std::unordered_set<UInt32>& seenActors, std::vector<ListenerCandidate>& out)
	{
		if (!actor || !actor->refID || !seenActors.insert(actor->refID).second)
			return;

		Actor* player = *g_thePlayerPtr;
		if (actor == player) {
			if ((flags & kFlag_SkipPlayer) || !(flags & kFlag_IncludePlayer))
				return;
		} else if (ActorIsTeammate(actor)) {
			return;
		}

		if (TESForm_GetDisabled(actor) || TESForm_GetDeleted(actor) || Engine::Actor_IsDead(actor, false))
			return;

		if (!actor->parentCell || !IsHearingProcess(static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor))))
			return;

		if (!IsSameSoundSpace(actor->parentCell, soundCell, flags))
			return;

		const float distSq = DistanceSquared(actor, location.x, location.y, location.z);
		if (distSq > radiusSq)
			return;

		out.push_back({actor->refID, distSq});
	}

	DetectionSoundLogic::ClassifiedCandidate ClassifyCandidate(const ListenerCandidate& cand)
	{
		using namespace DetectionSoundLogic;
		ClassifiedCandidate out = {cand.refID, cand.distSq, kCandidate_None, false};

		Actor* actor = LookupActor(cand.refID);
		Actor* player = *g_thePlayerPtr;
		if (!actor || actor == player || TESForm_GetDisabled(actor) || TESForm_GetDeleted(actor) || Engine::Actor_IsDead(actor, false))
			return out;

		auto* process = static_cast<BaseProcess*>(Engine::Actor_GetProcess(actor));
		if (!process)
			return out;
		out.canSpeak = process->processLevel == kProcessLevelHigh && !IsProcessSpeaking(process);

		int reaction = 0;
		const bool hostileToPlayer = player && Actor_GetShouldAttackActor(actor, player, false, &reaction, false);
		const bool undetected = player && Engine::Actor_GetDetectionValue(actor, player) <= 0;

		if (HasCombatTargets(actor)) {
			out.kind = (hostileToPlayer && undetected) ? kCandidate_ActiveCautionHostile : kCandidate_DetectedCombatHostile;
			return out;
		}

		if (IsSleepingState(GetSitSleepState(actor)) || Engine::Actor_GetCombatController(actor))
			return out;

		if (hostileToPlayer) {
			if (process->processLevel == kProcessLevelHigh && undetected)
				out.kind = kCandidate_IdleUndetectedHostile;
			return out;
		}

		out.kind = kCandidate_Neutral;
		return out;
	}

	UInt32 DispatchAnonymousSound(float x, float y, float z, UInt32 soundLevel, float radius, UInt32 flags,
		TESObjectCELL* cell, TESObjectREFR* sourceRef, TESTopic* explicitTopic)
	{
		if (radius <= 0.0f)
			return 0;

		auto* processManager = reinterpret_cast<ProcessManagerView*>(g_processManager);
		if (!processManager || !processManager->objects.data)
			return 0;

		const bool useDirectGreet = explicitTopic != nullptr;
		const bool useDistractionRules = (flags & (kFlag_FaceOrigin | kFlag_StartCautionSearch)) != 0 || useDirectGreet;
		const float searchZ = useDistractionRules ? SnapSearchZToGround(x, y, z) : z;
		BGSWorldLocationView rawLocation = {x, y, z, GetWorldLocationSpace(cell)};
		BGSWorldLocationView searchLocation = {x, y, searchZ, GetWorldLocationSpace(cell)};
		const float radiusSq = radius * radius;
		TESTopic* responseTopic = GetResponseTopic(explicitTopic, flags);
		const UInt32 sourceRefID = sourceRef ? sourceRef->refID : 0;

		std::unordered_set<UInt32> seenActors;
		std::vector<ListenerCandidate> candidates;
		seenActors.reserve(64);
		candidates.reserve(32);

		//collect refIDs before mutating AI/combat state - StartCombat and EvaluatePackage can mutate processManager->objects
		UInt32 upperBound = processManager->objects.firstFreeEntry;
		for (int bucket = 0; bucket < 2; ++bucket) {
			UInt32 begin = processManager->beginOffsets[bucket];
			UInt32 end = processManager->endOffsets[bucket];
			if (begin > upperBound) begin = upperBound;
			if (end > upperBound) end = upperBound;

			auto** current = processManager->objects.data + begin;
			auto** last = processManager->objects.data + end;
			for (; current < last; ++current) {
				auto* ref = reinterpret_cast<TESObjectREFR*>(*current);
				if (IsActorRef(ref))
					CollectListenerCandidate(static_cast<Actor*>(ref), cell, rawLocation, radiusSq, flags, seenActors, candidates);
			}
		}

		if (*g_thePlayerPtr)
			CollectListenerCandidate(*g_thePlayerPtr, cell, rawLocation, radiusSq, flags, seenActors, candidates);

		std::unordered_set<void*> seenGroups;
		seenGroups.reserve(16);

		const float strength = GetSearchStrength(soundLevel);
		if (!useDistractionRules) {
			UInt32 heardCount = 0;
			for (const auto& cand : candidates) {
				Actor* actor = LookupActor(cand.refID);
				if (!actor || TESForm_GetDisabled(actor) || TESForm_GetDeleted(actor) || Engine::Actor_IsDead(actor, false))
					continue;
				if (searchLocation.form)
					TryPushSearchLocation(actor, searchLocation, strength, seenGroups);
				actor = LookupActor(cand.refID);
				if (!actor)
					continue;
				AlertActorForSound(actor, flags, rawLocation, sourceRefID);
				if (responseTopic)
					QueueResponseBark(actor, responseTopic, false);
				++heardCount;
			}
			return heardCount;
		}

		std::vector<DetectionSoundLogic::ClassifiedCandidate> classified;
		classified.reserve(candidates.size());
		for (const auto& cand : candidates) {
			DetectionSoundLogic::ClassifiedCandidate info = ClassifyCandidate(cand);
			if (info.kind != DetectionSoundLogic::kCandidate_None)
				classified.push_back(info);
		}

		const UInt32 nowMs = GetTickCount();
		const DetectionSoundLogic::DistractionPlan plan = DetectionSoundLogic::BuildDistractionPlan(
			classified,
			searchLocation.form != nullptr && (flags & kFlag_StartCautionSearch) != 0,
			responseTopic != nullptr,
			g_reactionCooldowns,
			g_investigatorCooldowns,
			!g_pendingBarks.empty(),
			g_hasLastBark,
			g_lastBarkTick,
			nowMs);

		UInt32 heardCount = 0;
		UInt32 scoutOutRefID = 0;

		for (UInt32 combatRefID : plan.pushCombatSearchRefIDs) {
			if (Actor* actor = LookupActor(combatRefID)) {
				if (searchLocation.form)
					TryPushSearchLocation(actor, searchLocation, strength, seenGroups);
				++heardCount;
			}
		}

		if (plan.redirectScoutRefID) {
			if (TryRedirectCautionScout(plan.redirectScoutRefID, sourceRefID, searchLocation, strength, seenGroups)) {
				DetectionSoundLogic::StampActorCooldown(
					g_investigatorCooldowns, plan.redirectScoutRefID, nowMs, DetectionSoundLogic::kInvestigatorRedirectCooldownMs);
				scoutOutRefID = plan.redirectScoutRefID;
				++heardCount;
			}
		} else if (plan.newScoutRefID) {
			if (TryStartUndetectedCautionSearch(plan.newScoutRefID, sourceRefID, searchLocation, strength, seenGroups)) {
				DetectionSoundLogic::StampActorCooldown(
					g_investigatorCooldowns, plan.newScoutRefID, nowMs, DetectionSoundLogic::kInvestigatorRedirectCooldownMs);
				DetectionSoundLogic::StampActorCooldown(
					g_reactionCooldowns, plan.newScoutRefID, nowMs, DetectionSoundLogic::kNpcReactionCooldownMs);
				scoutOutRefID = plan.newScoutRefID;
				++heardCount;
			}
		}

		for (UInt32 turnRefID : plan.turnRefIDs) {
			Actor* actor = LookupActor(turnRefID);
			if (!actor || TESForm_GetDisabled(actor) || TESForm_GetDeleted(actor) || Engine::Actor_IsDead(actor, false))
				continue;
			DetectionSoundLogic::StampActorCooldown(
				g_reactionCooldowns, turnRefID, nowMs, DetectionSoundLogic::kNpcReactionCooldownMs);
			AlertActorForSound(actor, flags, searchLocation, sourceRefID);
			++heardCount;
		}

		if (plan.speakerRefID && responseTopic) {
			if (Actor* speaker = LookupActor(plan.speakerRefID))
				QueueResponseBark(speaker, responseTopic, useDirectGreet);
		}

		Log("DetectionSound: pos=(%.0f,%.0f,%.0f->%.0f) space=%08X rad=%.0f flags=%02X src=%08X cands=%u heard=%u scout=%08X turns=%u speaker=%08X",
			x, y, z, searchZ, searchLocation.form ? searchLocation.form->refID : 0, radius, flags, sourceRefID,
			static_cast<UInt32>(candidates.size()), heardCount, scoutOutRefID,
			static_cast<UInt32>(plan.turnRefIDs.size()), plan.speakerRefID);

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
		{"x", kParamType_Float, 0},
		{"y", kParamType_Float, 0},
		{"z", kParamType_Float, 0},
		{"sound level", kParamType_Integer, 0},
};

static ParamInfo kParams_CreateAnonymousDetectionSoundAt[7] =
	{
		{"x", kParamType_Float, 0},
		{"y", kParamType_Float, 0},
		{"z", kParamType_Float, 0},
		{"sound level", kParamType_Integer, 0},
		{"radius", kParamType_Float, 0},
		{"flags", kParamType_Integer, 1},
		{"response topic", kParamType_Topic, 1},
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
	*result = DispatchAnonymousSound(x, y, z, soundLevel, radius, flags, cell, thisObj, responseTopic);
	return true;
}

namespace DetectionSoundCommands {
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_CreateDetectionSoundAt);
		nvse->RegisterCommand(&kCommandInfo_CreateAnonymousDetectionSoundAt);
	}

	void Update()
	{
		if (!g_forcedAlerts.empty()) {
			const DWORD now = GetTickCount();
			for (size_t i = 0; i < g_forcedAlerts.size();) {
				ForcedAlert& alert = g_forcedAlerts[i];
				Actor* actor = LookupActor(alert.refID);
				const bool inCombat = actor && HasCombatTargets(actor);
				if (inCombat && alert.faceOrigin)
					ClearOriginFacing(actor, alert);

				if (!IsExpired(now, alert.expiresAt)) {
					if (actor && alert.faceOrigin && !inCombat && GetSitSleepState(actor) == 0 && actor->actorMover)
						ActorMover_SetLookAtPosition(actor->actorMover, alert.originX, alert.originY, alert.originZ);
					++i;
					continue;
				}

				if (inCombat && alert.clearAlertOnExpire) {
					//still fighting, keep the entry armed so the alert flag gets cleared by us once combat ends
					alert.expiresAt = now + GetAlertTimerMs();
					++i;
					continue;
				}

				ForcedAlert expired = alert;
				g_forcedAlerts[i] = g_forcedAlerts.back();
				g_forcedAlerts.pop_back();

				if (!inCombat) {
					if (expired.faceOrigin)
						ClearOriginFacing(actor, expired);
					if (actor && expired.clearAlertOnExpire && GetAlert(actor)) {
						SetAlert(actor, false);
						EvaluatePackage(actor, false, false);
					}
				}
			}
		}

		if (!g_pendingSearchPushes.empty()) {
			const DWORD now = GetTickCount();
			for (size_t i = 0; i < g_pendingSearchPushes.size();) {
				PendingSearchPush& push = g_pendingSearchPushes[i];
				if (push.framesLeft > 0) {
					--push.framesLeft;
					if (push.framesLeft == 0) {
						PendingSearchPush initial = push;
						Actor* actor = LookupActor(initial.actorRefID);
						Actor* player = *g_thePlayerPtr;
						TESForm* space = LookupWorldSpaceOrCell(initial.spaceRefID);
						if (actor && player && space && Engine::Actor_GetDetectionValue(actor, player) <= 0) {
							if (void* combatGroup = GetCombatGroup(actor)) {
								BGSWorldLocationView loc = {initial.x, initial.y, initial.z, space};
								ApplyScoutSearchLocation(actor, combatGroup, player, loc, initial.strength, false);
							}
						}
						if (!initial.awaitingSettle) {
							g_pendingSearchPushes[i] = g_pendingSearchPushes.back();
							g_pendingSearchPushes.pop_back();
							continue;
						}
					}
					++i;
					continue;
				}

				if (!IsExpired(now, push.settleAtMs)) {
					++i;
					continue;
				}

				PendingSearchPush settled = push;
				g_pendingSearchPushes[i] = g_pendingSearchPushes.back();
				g_pendingSearchPushes.pop_back();

				TESObjectREFR* sourceRef = LookupRef(settled.sourceRefID);
				if (!sourceRef || TESForm_GetDeleted(sourceRef) || TESForm_GetDisabled(sourceRef) || !sourceRef->parentCell)
					continue;

				const float landedX = sourceRef->posX;
				const float landedY = sourceRef->posY;
				const float landedZ = SnapSearchZToGround(landedX, landedY, sourceRef->posZ);
				TESForm* landedSpace = GetWorldLocationSpace(sourceRef->parentCell);

				for (auto& alert : g_forcedAlerts) {
					if (alert.faceOrigin && alert.headTrackRefID == settled.sourceRefID) {
						alert.originX = landedX;
						alert.originY = landedY;
						alert.originZ = landedZ;
					}
				}

				Actor* actor = LookupActor(settled.actorRefID);
				Actor* player = *g_thePlayerPtr;
				if (actor && player && landedSpace && Engine::Actor_GetDetectionValue(actor, player) <= 0) {
					if (void* combatGroup = GetCombatGroup(actor)) {
						BGSWorldLocationView loc = {landedX, landedY, landedZ, landedSpace};
						ApplyScoutSearchLocation(actor, combatGroup, player, loc, settled.strength, true);
						Log("DetectionSound: settled scout=%08X src=%08X pos=(%.0f,%.0f,%.0f) space=%08X",
							settled.actorRefID, settled.sourceRefID, landedX, landedY, landedZ, landedSpace->refID);
					}
				}
			}
		}

		if (!g_pendingBarks.empty()) {
			std::vector<PendingBark> pendingBarks;
			pendingBarks.swap(g_pendingBarks);

			for (const auto& bark : pendingBarks) {
				Actor* actor = LookupActor(bark.speakerRefID);
				auto* form = static_cast<TESForm*>(Engine::LookupFormByID(bark.topicRefID));
				auto* topic = (form && form->typeID == kFormType_DIAL) ? static_cast<TESTopic*>(form) : nullptr;
				if (actor && topic)
					StartResponseBark(actor, topic, bark.useProcessGreet);
			}
		}
	}

	void ClearState()
	{
		g_forcedAlerts.clear();
		g_pendingBarks.clear();
		g_pendingSearchPushes.clear();
		g_reactionCooldowns.clear();
		g_investigatorCooldowns.clear();
		g_lastBarkTick = 0;
		g_hasLastBark = false;
	}
}
