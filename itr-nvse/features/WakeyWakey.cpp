//native port of the WakeyWakey script mod - nearby sleeping NPCs wake on player gunfire
//hooks the weapon-fire call, scans high-process actors, wakes sleepers

#include "WakeyWakey.h"

#include <Windows.h>

#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/Detours.h"
#include "internal/layout/Process.h"

extern void Log(const char* fmt, ...);

namespace {

constexpr UInt32 kFormID_AvoidThreat = 0xED; //base-game AvoidThreat DIAL, load-order stable
constexpr int kMaxWokenPerShot = 16;

using FireWeapon_t = void(__thiscall*)(TESObjectWEAP*, Actor*);
using SetAlerted_t = void(__thiscall*)(Actor*, UInt8);
using StartCombat_t = void(__thiscall*)(Actor*, Actor*, void*, char, char, char, UInt32, char, void*);
using GetSitSleepState_t = int(__thiscall*)(Actor*);
using StopCurrentDialogue_t = void(__thiscall*)(Actor*);
using SetDialogueTarget_t = void(__thiscall*)(Actor*, Actor*);
using ProcessSetByte_t = void(__thiscall*)(void*, UInt8);
using ProcessSetActor_t = void(__thiscall*)(void*, Actor*);
using ProcessNoArgs_t = void(__thiscall*)(void*);
using ProcessGreet_t = void(__thiscall*)(void*, Actor*, TESTopic*, bool, bool, bool, bool);
//StopCombat script command handler, numParams 0 so it reads only thisObj (3rd COMMAND_ARG)
using StopCombatCmd_t = char(__cdecl*)(void*, void*, TESObjectREFR*, void*, void*, void*, double*, void*);

static const SetAlerted_t Actor_SetAlerted = (SetAlerted_t)0x8A5E40;   //Actor::SetAlerted, SetAlert command call
static const StartCombat_t Actor_StartCombat = (StartCombat_t)0x89FCF0; //Actor::StartCombat, wakes via InitGetUpPackage
static const StopCombatCmd_t StopCombatCommand = (StopCombatCmd_t)0x5C1A50; //vanilla StopCombat command handler
static const StopCurrentDialogue_t Actor_StopCurrentDialogue = (StopCurrentDialogue_t)0x934250;
static const SetDialogueTarget_t Actor_SetDialogueTarget = (SetDialogueTarget_t)0x57BD60;
static const ProcessGreet_t ProcessGreet = (ProcessGreet_t)0x8DBE30;

Detours::CallDetour g_fireDetour;
FireWeapon_t g_origFireWeapon = nullptr;
bool g_enabled = false;   //hook installs once, this gates its scan so reload can toggle it

float g_wakeDistance = 2500.0f;
float g_quietWakeDistance = 1250.0f;
DWORD g_cooldownMs = 250;
DWORD g_mainThreadId = 0;
DWORD g_lastScanTick = 0;
TESTopic* g_avoidThreatTopic = nullptr;

int GetSitSleepState(Actor* actor)
{
	void** vtbl = *reinterpret_cast<void***>(actor);
	return reinterpret_cast<GetSitSleepState_t>(vtbl[0x214 / 4])(actor); //Actor::GetSitSleepState
}

bool IsWakeableActor(Actor* actor)
{
	if (!actor) return false;
	if (actor->typeID != kFormType_ACHR && actor->typeID != kFormType_ACRE) return false;
	if (Engine::Actor_IsDead(actor, false)) return false;
	if (!actor->renderState || !actor->renderState->niNode) return false; //unloaded or disabled
	//raw vtbl+0x214 state, 1-5 sitting chain, 6-9 bed chain, 9 fully asleep in bed
	//10 already wants wake and gets up on its own, GetSleeping maps raw 9 to script 3
	if (GetSitSleepState(actor) != 9) return false;
	if (Engine::Actor_GetCombatController(actor)) return false; //already fighting
	return true;
}

void SetProcessDialogueTarget(void* process, Actor* target)
{
	auto** vtbl = *reinterpret_cast<void***>(process);
	reinterpret_cast<ProcessSetByte_t>(vtbl[0x648 / sizeof(void*)])(process, 0);
	reinterpret_cast<ProcessSetActor_t>(vtbl[0x628 / sizeof(void*)])(process, target);
}

bool SayTo(Actor* speaker, TESTopic* topic, Actor* target)
{
	if (!speaker || !topic || !target)
		return false;

	auto* speakerProcess = static_cast<BaseProcess*>(Engine::Actor_GetProcess(speaker));
	auto* targetProcess = static_cast<BaseProcess*>(Engine::Actor_GetProcess(target));
	if (!speakerProcess || !targetProcess || speakerProcess->processLevel != 0)
		return false;

	UInt32* lipDistance = GetVoiceLipDistanceLimit();
	UInt32 oldLipDistance = *lipDistance;
	*lipDistance = 0x7FFFFFFF;

	Actor_StopCurrentDialogue(speaker);

	auto** speakerVtbl = *reinterpret_cast<void***>(speakerProcess);
	reinterpret_cast<ProcessSetByte_t>(speakerVtbl[0x370 / sizeof(void*)])(speakerProcess, 0);
	reinterpret_cast<ProcessNoArgs_t>(speakerVtbl[0x224 / sizeof(void*)])(speakerProcess);

	SetProcessDialogueTarget(targetProcess, speaker);
	Actor_SetDialogueTarget(target, speaker);
	SetProcessDialogueTarget(speakerProcess, target);
	Actor_SetDialogueTarget(speaker, target);
	ProcessGreet(speakerProcess, speaker, topic, false, false, true, true);

	*lipDistance = oldLipDistance;
	return true;
}

void WakeActor(Actor* actor, Actor* player)
{
	Actor_SetAlerted(actor, 1);
	Actor_StartCombat(actor, player, nullptr, 1, 1, 0, 0, 0, nullptr);
	double dummy = 0.0;
	StopCombatCommand(nullptr, nullptr, reinterpret_cast<TESObjectREFR*>(actor), nullptr, nullptr, nullptr, &dummy, nullptr);
	Actor_SetAlerted(actor, 0);

	SayTo(actor, g_avoidThreatTopic, player);

	if (g_eventManagerInterface) {
		g_eventManagerInterface->DispatchEvent("WakeyWakeyNPC", reinterpret_cast<TESObjectREFR*>(actor),
			reinterpret_cast<TESForm*>(actor));
		g_eventManagerInterface->DispatchEvent("ITR:OnNPCWokeByGunfire", reinterpret_cast<TESObjectREFR*>(actor),
			reinterpret_cast<TESForm*>(actor), reinterpret_cast<TESForm*>(player));
	}
}

void ScanAndWake(Actor* player, TESObjectWEAP* weapon)
{
	DWORD now = GetTickCount();
	if (now - g_lastScanTick < g_cooldownMs) return;
	g_lastScanTick = now;

	if (!weapon) return;
	UInt32 soundLevel = weapon->soundLevel; //0x364 - 0 loud, 1 quiet, 2 silent
	if (soundLevel == 2) return;
	float radius = (soundLevel == 1) ? g_quietWakeDistance : g_wakeDistance;
	float radiusSq = radius * radius;

	if (!g_avoidThreatTopic)
		g_avoidThreatTopic = reinterpret_cast<TESTopic*>(Engine::LookupFormByID(kFormID_AvoidThreat));

	auto* pm = reinterpret_cast<ProcessManagerLite*>(g_processManager);
	if (!pm || !pm->objects.data) return;

	float px = player->posX;
	float py = player->posY;

	UInt32 upper = pm->objects.firstFreeEntry;
	UInt32 begin = pm->beginOffsets[0]; //bucket 0 = high process
	UInt32 end = pm->endOffsets[0];
	if (begin > upper) begin = upper;
	if (end > upper) end = upper;

	//collect refIDs first - WakeActor's StartCombat and script event handlers can mutate
	//the process lists, so we must not dispatch while walking the live array
	UInt32 wakeIDs[kMaxWokenPerShot];
	int wakeCount = 0;
	UInt32 wakeableCount = 0;
	for (UInt32 i = begin; i < end && wakeCount < kMaxWokenPerShot; i++) {
		auto* actor = reinterpret_cast<Actor*>(pm->objects.data[i]);
		if (!IsWakeableActor(actor)) continue;
		wakeableCount++;

		float dx = actor->posX - px;
		float dy = actor->posY - py;
		if (dx * dx + dy * dy > radiusSq) continue;

		wakeIDs[wakeCount++] = actor->refID;
	}
	Log("WakeyWakey: scan high=%u wakeable=%u nearby=%d sound=%u", end - begin, wakeableCount, wakeCount, soundLevel);

	UInt32 playerID = player->refID;
	for (int i = 0; i < wakeCount; i++) {
		auto* actor = reinterpret_cast<Actor*>(Engine::LookupFormByID(wakeIDs[i]));
		if (!actor || !IsWakeableActor(actor)) continue; //re-validate, a prior wake may have changed state
		auto* pc = reinterpret_cast<Actor*>(Engine::LookupFormByID(playerID));
		if (!pc) break;
		WakeActor(actor, pc);
		Log("WakeyWakey: woke %08X", wakeIDs[i]);
	}
}

void __fastcall Hook_FireWeapon(TESObjectWEAP* weapon, void* /*edx*/, Actor* actor)
{
	if (g_enabled) {
		if (GetCurrentThreadId() != g_mainThreadId) {
			static volatile LONG loggedOffThread = 0;
			if (InterlockedCompareExchange(&loggedOffThread, 1, 0) == 0)
				Log("WakeyWakey: fire hook ran off main thread, skipping scan");
		} else {
			void** playerPtr = reinterpret_cast<void**>(g_thePlayerPtr);
			if (playerPtr && actor == reinterpret_cast<Actor*>(*playerPtr))
				ScanAndWake(actor, weapon);
		}
	}

	g_origFireWeapon(weapon, actor);
}

} //namespace

namespace WakeyWakey {

void Init(bool enable, float wakeDistance, float quietWakeDistance, int cooldownMs)
{
	//third-party script mod ships this file; native port stands down to avoid double-waking
	if (GetFileAttributesA("Data\\NVSE\\Plugins\\scripts\\ln_wakey_wakey.txt") != INVALID_FILE_ATTRIBUTES) {
		Log("WakeyWakey: script mod detected, native port disabled");
		return;
	}

	//install the fire hook regardless of the startup flag and gate its scan on g_enabled,
	//so ReloadPluginConfig can turn the feature on or off without a restart
	g_enabled = enable;
	g_wakeDistance = wakeDistance;
	g_quietWakeDistance = quietWakeDistance;
	g_cooldownMs = cooldownMs > 0 ? (DWORD)cooldownMs : 0;
	g_mainThreadId = GetCurrentThreadId();

	if (g_eventManagerInterface) {
		using P = NVSEEventManagerInterface::ParamType;
		using F = NVSEEventManagerInterface::EventFlags;
		static P oneForm[] = { P::eParamType_AnyForm };
		static P twoForms[] = { P::eParamType_AnyForm, P::eParamType_AnyForm };
		g_eventManagerInterface->RegisterEvent("WakeyWakeyNPC", 1, oneForm, F::kFlag_FlushOnLoad);
		g_eventManagerInterface->RegisterEvent("ITR:OnNPCWokeByGunfire", 2, twoForms, F::kFlag_FlushOnLoad);
	}

	if (g_fireDetour.WriteRelCall(0x8BADE9, Hook_FireWeapon)) {
		g_origFireWeapon = reinterpret_cast<FireWeapon_t>(g_fireDetour.GetOverwrittenAddr());
		Log("WakeyWakey: installed (wake %.0f, quiet %.0f, cooldown %ums)", wakeDistance, quietWakeDistance, g_cooldownMs);
	} else {
		Log("WakeyWakey: fire hook install failed at 0x8BADE9");
	}
}

void UpdateSettings(bool enable, float wakeDistance, float quietWakeDistance, int cooldownMs)
{
	g_enabled = enable;   //inert if Init stood down for the script mod, no hook is installed then
	g_wakeDistance = wakeDistance;
	g_quietWakeDistance = quietWakeDistance;
	g_cooldownMs = cooldownMs > 0 ? (DWORD)cooldownMs : 0;
}

}
