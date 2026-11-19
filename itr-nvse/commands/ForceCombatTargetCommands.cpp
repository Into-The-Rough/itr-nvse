//ForceCombatTarget - force an actor onto a specific combat target via hooked target selection

#include "ForceCombatTargetCommands.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/layout/Combat.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <unordered_map>

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

static bool IsActorRef(TESObjectREFR* ref)
{
	if (!ref || !ref->baseForm) return false;
	return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
}

using GetCombatTargetForActor_t = void*(__thiscall*)(void* combatGroup, Actor* target);
using CombatGroupCanAddTarget_t = bool(__thiscall*)(void* combatGroup, Actor* target);
using CharacterIsGuardForForceCombatTarget_t = UInt8(__thiscall*)(Character*);
using CombatControllerSetTarget_t = void(__thiscall*)(void* combatController, Actor* target);
using CombatControllerAddCombatTarget_t = void(__thiscall*)(void* combatController, Actor* target, SInt32 a3, SInt32 a4, float a5, float a6);
using CombatManagerAddCombatant_t = void*(__thiscall*)(void* combatManager, Actor* actor, Actor* target, SInt32 a4, SInt32 a5);
using ActorStartCombat_t = void(__thiscall*)(Actor* actor, Actor* target, void* combatGroup, bool ignoreActorLimit, bool isAggressor, bool a6, UInt32 a7, bool a8, TESPackage* package);
using ActorPutCreatedPackage_t = void(__thiscall*)(Actor* actor, void* package, UInt32 unk1, UInt32 unk2);
using ProcessComputeLastTimeProcessed_t = void(__thiscall*)(void* process);
using ProcessSavePackageToExtraData_t = void(__thiscall*)(void* process, Actor* actor);
using CombatControllerSetByte0C4_t = void(__thiscall*)(void* combatController);
static const GetCombatTargetForActor_t GetCombatTargetForActor = (GetCombatTargetForActor_t)0x9865D0;
static const CombatGroupCanAddTarget_t CombatGroupCanAddTarget = (CombatGroupCanAddTarget_t)0x9866D0;
static const CharacterIsGuardForForceCombatTarget_t CharacterIsGuardForForceCombatTarget = (CharacterIsGuardForForceCombatTarget_t)0x8D1ED0;
static const CombatControllerSetTarget_t CombatControllerSetTarget = (CombatControllerSetTarget_t)0x980830;
static const CombatControllerAddCombatTarget_t CombatControllerAddCombatTarget = (CombatControllerAddCombatTarget_t)0x97F930;
static const CombatManagerAddCombatant_t CombatManagerAddCombatant = (CombatManagerAddCombatant_t)0x992110;
static const ActorStartCombat_t ActorStartCombat = (ActorStartCombat_t)0x89FCF0;
static const ActorPutCreatedPackage_t ActorPutCreatedPackage = (ActorPutCreatedPackage_t)0x87EAC0;
static const ProcessComputeLastTimeProcessed_t ProcessComputeLastTimeProcessed = (ProcessComputeLastTimeProcessed_t)0x907650;
static const ProcessSavePackageToExtraData_t ProcessSavePackageToExtraData = (ProcessSavePackageToExtraData_t)0x9130F0;
static const CombatControllerSetByte0C4_t CombatControllerSetByte0C4 = (CombatControllerSetByte0C4_t)0x8A0250;

namespace {
	using EvaluateCombatTargets_t = Actor*(__thiscall*)(void* combatGroup, Actor* actor);
	using CanAttackActor_t = bool(__thiscall*)(Actor* actor, Actor* target);
	static constexpr UInt32 kAddr_ActorCanAttackActor = 0x8B0670;
	static const CanAttackActor_t ActorCanAttackActor = reinterpret_cast<CanAttackActor_t>(kAddr_ActorCanAttackActor);

	enum class ForceCombatTargetResult {
		kSuccess,
		kInvalidArgs,
		kNoProcess,
		kActorDead,
		kHookFailed,
		kNoCombatController,
		kNoCombatGroup,
		kCannotAddTarget,
		kAddTargetFailed,
	};

	static CRITICAL_SECTION g_forceCombatTargetLock;
	static volatile LONG g_forceCombatTargetLockInit = 0;
	static std::unordered_map<UInt32, UInt32> g_forcedCombatTargets;
	//cheap idle gate so the hooks skip all map/lock/engine work when nothing is forced
	static volatile bool s_hasForcedTargets = false;
	static Detours::JumpDetour s_forceCombatTargetDetour;
	static Detours::JumpDetour s_forceCombatCanAttackDetour;
	static EvaluateCombatTargets_t s_evaluateCombatTargetsOriginal = nullptr;
	static CanAttackActor_t s_canAttackActorOriginal = nullptr;
	static bool g_forceCombatTargetHookInstalled = false;
	static UInt32 GetForcedCombatTargetRefID(UInt32 actorRefID);

	static bool IsForcedCombatTargetPair(Actor* actor, Actor* target)
	{
		if (!actor || !target || actor == target)
			return false;
		//map lookup first so dead-checks only run for pairs we actually forced
		if (GetForcedCombatTargetRefID(actor->refID) != target->refID)
			return false;
		return !Engine::Actor_IsDead(actor, false) && !Engine::Actor_IsDead(target, false);
	}

	static void* TryAddCombatantBootstrap(Actor* actor, Actor* target, bool ignoreActorLimit)
	{
		void* combatManager = GetCombatManager();
		if (!actor || !target || !combatManager)
			return nullptr;

		//call the patched entry so forced pairs pass the bootstrap precheck
		if (!ActorCanAttackActor(actor, target))
			return nullptr;

		void* combatController = CombatManagerAddCombatant(combatManager, actor, target, 0, 0);
		if (!combatController)
			return nullptr;

		if (ignoreActorLimit)
			CombatControllerSetByte0C4(combatController);

		void* process = Engine::Actor_GetProcess(actor);
		if (process) {
			ProcessComputeLastTimeProcessed(process);
			ProcessSavePackageToExtraData(process, actor);
		}

		ActorPutCreatedPackage(actor, combatController, 0, 1);
		actor->unk104 = 1;
		return combatController;
	}

	static void EnsureForceCombatTargetLockInit()
	{
		InitCriticalSectionOnce(&g_forceCombatTargetLockInit, &g_forceCombatTargetLock);
	}

	static UInt32 GetForcedCombatTargetRefID(UInt32 actorRefID)
	{
		if (!actorRefID || g_forceCombatTargetLockInit != 2)
			return 0;

		ScopedLock lock(&g_forceCombatTargetLock);
		auto it = g_forcedCombatTargets.find(actorRefID);
		return it != g_forcedCombatTargets.end() ? it->second : 0;
	}

	static void SetForcedCombatTargetRefID(UInt32 actorRefID, UInt32 targetRefID)
	{
		if (!actorRefID || !targetRefID)
			return;

		EnsureForceCombatTargetLockInit();
		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets[actorRefID] = targetRefID;
		s_hasForcedTargets = true;
	}

	static void ClearForcedCombatTargetRefID(UInt32 actorRefID)
	{
		if (!actorRefID || g_forceCombatTargetLockInit != 2)
			return;

		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets.erase(actorRefID);
		s_hasForcedTargets = !g_forcedCombatTargets.empty();
	}

	static Actor* __fastcall Hook_EvaluateCombatTargets(void* combatGroup, void* edx, Actor* actor)
	{
		if (!s_hasForcedTargets || !combatGroup || !actor)
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);

		UInt32 forcedTargetRefID = GetForcedCombatTargetRefID(actor->refID);
		if (!forcedTargetRefID)
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);

		TESForm* forcedForm = (TESForm*)Engine::LookupFormByID(forcedTargetRefID);
		Actor* forcedTarget = forcedForm && IsActorRef((TESObjectREFR*)forcedForm) ? (Actor*)forcedForm : nullptr;
		if (!forcedTarget || forcedTarget == actor || Engine::Actor_IsDead(forcedTarget, false)) {
			ClearForcedCombatTargetRefID(actor->refID);
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);
		}

		if (GetCombatTargetForActor(combatGroup, forcedTarget))
			return forcedTarget;

		return s_evaluateCombatTargetsOriginal(combatGroup, actor);
	}

	static bool __fastcall Hook_CanAttackActor(Actor* actor, void* edx, Actor* target)
	{
		//zero-cost vanilla path while no targets are forced, stale flag read is benign
		if (!s_hasForcedTargets)
			return s_canAttackActorOriginal(actor, target);
		if (IsForcedCombatTargetPair(actor, target))
			return true;
		return s_canAttackActorOriginal(actor, target);
	}

	//installed once from Init at plugin load, never mid-game
	//trampolineOut publishes the original before the jmp goes live so the hooks can call it unconditionally
	static bool InstallForceCombatTargetHooks()
	{
		if (!s_forceCombatTargetDetour.WriteRelJump(0x986C60, Hook_EvaluateCombatTargets, 10,
				(UInt8**)&s_evaluateCombatTargetsOriginal))
			return false;

		if (!s_forceCombatCanAttackDetour.WriteRelJump(kAddr_ActorCanAttackActor, Hook_CanAttackActor, 6,
				(UInt8**)&s_canAttackActorOriginal)) {
			if (s_forceCombatTargetDetour.Remove())
				s_evaluateCombatTargetsOriginal = nullptr;
			return false;
		}

		g_forceCombatTargetHookInstalled = true;
		return true;
	}

	static const char* ForceCombatTargetResultToString(ForceCombatTargetResult result)
	{
		switch (result) {
			case ForceCombatTargetResult::kSuccess:
				return "success";
			case ForceCombatTargetResult::kInvalidArgs:
				return "invalid actor/target";
			case ForceCombatTargetResult::kNoProcess:
				return "actor or target has no current process";
			case ForceCombatTargetResult::kActorDead:
				return "actor or target is dead";
			case ForceCombatTargetResult::kHookFailed:
				return "target-selection hook unavailable";
			case ForceCombatTargetResult::kNoCombatController:
				return "failed to create or get combat controller";
			case ForceCombatTargetResult::kNoCombatGroup:
				return "combat controller has no combat group";
			case ForceCombatTargetResult::kCannotAddTarget:
				return "combat group rejected target (likely faction/aggression relation)";
			case ForceCombatTargetResult::kAddTargetFailed:
				return "target was not added to combat group";
			default:
				return "unknown";
		}
	}

	static ForceCombatTargetResult TryForceCombatTarget(Actor* actor, Actor* target)
	{
		if (!actor || !target || actor == target)
			return ForceCombatTargetResult::kInvalidArgs;
		if (!Engine::Actor_GetProcess(actor) || !Engine::Actor_GetProcess(target))
			return ForceCombatTargetResult::kNoProcess;
		if (Engine::Actor_IsDead(actor, false) || Engine::Actor_IsDead(target, false))
			return ForceCombatTargetResult::kActorDead;
		//hooks are installed eagerly at plugin load, just check the result
		if (!g_forceCombatTargetHookInstalled)
			return ForceCombatTargetResult::kHookFailed;
		SetForcedCombatTargetRefID(actor->refID, target->refID);

		void* combatController = Engine::Actor_GetCombatController(actor);
		if (!combatController) {
			const bool ignoreActorLimit = true;
			bool isGuard = actor->baseForm && actor->baseForm->typeID == kFormType_NPC
				&& CharacterIsGuardForForceCombatTarget((Character*)actor) != 0;
			ActorStartCombat(actor, target, nullptr, ignoreActorLimit, !isGuard, false, 0, true, nullptr);
			combatController = Engine::Actor_GetCombatController(actor);
			if (!combatController)
				combatController = TryAddCombatantBootstrap(actor, target, ignoreActorLimit);
		}

		if (!combatController) {
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kNoCombatController;
		}

		void* combatGroup = CombatControllerGetCombatGroup(combatController);
		if (!combatGroup) {
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kNoCombatGroup;
		}

		if (!GetCombatTargetForActor(combatGroup, target)) {
			if (!CombatGroupCanAddTarget(combatGroup, target)) {
				ClearForcedCombatTargetRefID(actor->refID);
				return ForceCombatTargetResult::kCannotAddTarget;
			}
			CombatControllerAddCombatTarget(combatController, target, 0, 0, 0.0f, 0.0f);
		}

		if (!GetCombatTargetForActor(combatGroup, target)) {
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kAddTargetFailed;
		}

		CombatControllerSetTarget(combatController, target);
		return ForceCombatTargetResult::kSuccess;
	}

	static void ClearForcedCombatTarget(Actor* actor)
	{
		if (!actor)
			return;

		ClearForcedCombatTargetRefID(actor->refID);

		void* combatController = Engine::Actor_GetCombatController(actor);
		if (!combatController)
			return;

		void* combatGroup = CombatControllerGetCombatGroup(combatController);
		if (!combatGroup || !s_evaluateCombatTargetsOriginal)
			return;

		CombatControllerSetTarget(combatController, s_evaluateCombatTargetsOriginal(combatGroup, actor));
	}
}

static ParamInfo kParams_ForceCombatTarget[1] = {
	{"target", kParamType_Actor, 1},
};
DEFINE_COMMAND_PLUGIN(ForceCombatTarget, "Force actor to target a specific combat target; pass 0 to clear", 1, 1, kParams_ForceCombatTarget);

bool Cmd_ForceCombatTarget_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj || !IsActorRef(thisObj))
		return true;

	Actor* actor = (Actor*)thisObj;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target))
		return true;

	if (!target) {
		ClearForcedCombatTarget(actor);
		*result = 1;
		if (IsConsoleMode())
			Console_Print("ForceCombatTarget >> cleared");
		return true;
	}

	ForceCombatTargetResult forceResult = TryForceCombatTarget(actor, target);
	if (forceResult != ForceCombatTargetResult::kSuccess) {
		if (IsConsoleMode())
			Console_Print("ForceCombatTarget >> failed: %s", ForceCombatTargetResultToString(forceResult));
		return true;
	}

	*result = 1;
	if (IsConsoleMode())
		Console_Print("ForceCombatTarget >> %08X", target->refID);
	return true;
}

namespace ForceCombatTargetCommands {

	void Init()
	{
		EnsureForceCombatTargetLockInit();
		if (!InstallForceCombatTargetHooks())
			Log("ForceCombatTarget: hook install failed (site already patched by another mod?)");
	}

	void ClearState()
	{
		if (g_forceCombatTargetLockInit == 2) {
			ScopedLock lock(&g_forceCombatTargetLock);
			g_forcedCombatTargets.clear();
			s_hasForcedTargets = false;
		}
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
		nvse->RegisterCommand(&kCommandInfo_ForceCombatTarget);
	}
}
