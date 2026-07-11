//native port of the WakeyWakey script mod - nearby sleeping NPCs wake on player gunfire
//hooks the ammo-consume call inside the fire handler, scans high-process actors, wakes sleepers

#include "WakeyWakey.h"

#include <Windows.h>

#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/Detours.h"
#include "internal/layout/Process.h"

#include "commands/ForceSayCommand.h"

extern void Log(const char* fmt, ...);

namespace {

constexpr UInt32 kFormID_AvoidThreat = 0xED; //base-game AvoidThreat DIAL, load-order stable
constexpr int kMaxWokenPerShot = 16;

using RemoveAmmoOnFire_t = int(__thiscall*)(Actor*, int);
using SetAlerted_t = void(__thiscall*)(Actor*, UInt8);
using StartCombat_t = void(__thiscall*)(Actor*, Actor*, void*, char, char, char, UInt32, char, void*);
using GetSitSleepState_t = int(__thiscall*)(Actor*);
//StopCombat script command handler, numParams 0 so it reads only thisObj (3rd COMMAND_ARG)
using StopCombatCmd_t = char(__cdecl*)(void*, void*, TESObjectREFR*, void*, void*, void*, double*, void*);

static const SetAlerted_t Actor_SetAlerted = (SetAlerted_t)0x8A5E40;   //Actor::SetAlerted, SetAlert command call
static const StartCombat_t Actor_StartCombat = (StartCombat_t)0x89FCF0; //Actor::StartCombat, wakes via InitGetUpPackage
static const StopCombatCmd_t StopCombatCommand = (StopCombatCmd_t)0x5C1A50; //vanilla StopCombat command handler

Detours::CallDetour g_fireDetour;
RemoveAmmoOnFire_t g_origRemoveAmmo = nullptr;

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
	if (GetSitSleepState(actor) != 3) return false; //3 = fully sleeping in bed
	if (Engine::Actor_GetCombatController(actor)) return false; //already fighting
	return true;
}

void WakeActor(Actor* actor, Actor* player)
{
	Actor_SetAlerted(actor, 1);
	Actor_StartCombat(actor, player, nullptr, 1, 1, 0, 0, 0, nullptr);
	double dummy = 0.0;
	StopCombatCommand(nullptr, nullptr, reinterpret_cast<TESObjectREFR*>(actor), nullptr, nullptr, nullptr, &dummy, nullptr);
	Actor_SetAlerted(actor, 0);

	ForceSayCommand::ForceSay(actor, g_avoidThreatTopic, player);

	if (g_eventManagerInterface) {
		g_eventManagerInterface->DispatchEvent("WakeyWakeyNPC", reinterpret_cast<TESObjectREFR*>(actor),
			reinterpret_cast<TESForm*>(actor));
		g_eventManagerInterface->DispatchEvent("ITR:OnNPCWokeByGunfire", reinterpret_cast<TESObjectREFR*>(actor),
			reinterpret_cast<TESForm*>(actor), reinterpret_cast<TESForm*>(player));
	}
}

void ScanAndWake(Actor* player)
{
	DWORD now = GetTickCount();
	if (now - g_lastScanTick < g_cooldownMs) return;
	g_lastScanTick = now;

	auto* weapon = reinterpret_cast<TESObjectWEAP*>(Engine::Actor_GetEquippedWeapon(player));
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
	for (UInt32 i = begin; i < end && wakeCount < kMaxWokenPerShot; i++) {
		auto* actor = reinterpret_cast<Actor*>(pm->objects.data[i]);
		if (!IsWakeableActor(actor)) continue;

		float dx = actor->posX - px;
		float dy = actor->posY - py;
		if (dx * dx + dy * dy > radiusSq) continue;

		wakeIDs[wakeCount++] = actor->refID;
	}

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

int __fastcall Hook_RemoveAmmoOnFire(Actor* actor, void* /*edx*/, int shotCount)
{
	int result = g_origRemoveAmmo(actor, shotCount);

	if (GetCurrentThreadId() != g_mainThreadId) {
		static volatile LONG loggedOffThread = 0;
		if (InterlockedCompareExchange(&loggedOffThread, 1, 0) == 0)
			Log("WakeyWakey: fire hook ran off main thread, skipping scan");
		return result;
	}

	void** playerPtr = reinterpret_cast<void**>(g_thePlayerPtr);
	if (playerPtr && actor == reinterpret_cast<Actor*>(*playerPtr))
		ScanAndWake(actor);

	return result;
}

} //namespace

namespace WakeyWakey {

void Init(bool enable, float wakeDistance, float quietWakeDistance, int cooldownMs)
{
	if (!enable) return;

	//third-party script mod ships this file; native port stands down to avoid double-waking
	if (GetFileAttributesA("Data\\NVSE\\Plugins\\scripts\\ln_wakey_wakey.txt") != INVALID_FILE_ATTRIBUTES) {
		Log("WakeyWakey: script mod detected, native port disabled");
		return;
	}

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

	if (g_fireDetour.WriteRelCall(0x95DE16, Hook_RemoveAmmoOnFire)) {
		g_origRemoveAmmo = reinterpret_cast<RemoveAmmoOnFire_t>(g_fireDetour.GetOverwrittenAddr());
		Log("WakeyWakey: installed (wake %.0f, quiet %.0f, cooldown %ums)", wakeDistance, quietWakeDistance, g_cooldownMs);
	} else {
		Log("WakeyWakey: fire hook install failed at 0x95DE16");
	}
}

}
