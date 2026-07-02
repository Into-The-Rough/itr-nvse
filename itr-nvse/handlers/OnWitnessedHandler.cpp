//ITR:OnWitnessed - hook Crime::AddtoActorKnowList (0x9EB9C0), fired per witness.
//trespass bypasses Crime, so also hook Actor::TrespassAlarm (0x8C0EC0).

#include "OnWitnessedHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/WitnessScan.h"
#include "internal/globals.h"
#include "internal/GameGlobals.h"
#include <vector>
#include <Windows.h>

static Actor* GetPlayerActor() { return *(Actor**)g_thePlayerPtr; }

//TrespassAlarm re-fires every AI tick while the player loiters in an owned space, so
//rate-limit the event per witness rather than dispatching on every tick
static UInt32 s_witnessIds[32] = {0};
static UInt32 s_witnessLastMs[32] = {0};

static bool TrespassWitnessReady(UInt32 witnessID, UInt32 now)
{
	constexpr UInt32 kWindowMs = 3000;

	UInt32 oldest = 0;
	for (UInt32 i = 0; i < 32; ++i) {
		if (s_witnessIds[i] == witnessID) {
			if (now - s_witnessLastMs[i] < kWindowMs) return false;
			s_witnessLastMs[i] = now;
			return true;
		}
		if (now - s_witnessLastMs[i] > now - s_witnessLastMs[oldest])
			oldest = i;
	}
	s_witnessIds[oldest] = witnessID;
	s_witnessLastMs[oldest] = now;
	return true;
}

constexpr UInt32 kAddr_AddtoActorKnowList = 0x9EB9C0;
constexpr UInt32 kAddr_TrespassAlarm      = 0x8C0EC0;

static Detours::JumpDetour s_addKnowDetour;
static Detours::JumpDetour s_trespassDetour;

using AddKnow_t   = void (__thiscall*)(Engine::Crime*, Actor*);
using Trespass_t  = UInt32 (__thiscall*)(Actor*, TESObjectREFR*, TESForm*, UInt32);

static void DispatchWitnessedEvent(Actor* witness, Actor* criminal,
                                   UInt32 crimeType, TESObjectREFR* victim,
                                   SInt32 detectionValue)
{
	if (!g_eventManagerInterface || !witness) return;

	g_eventManagerInterface->DispatchEvent("ITR:OnWitnessed",
		reinterpret_cast<TESObjectREFR*>(witness),  //thisObj
		reinterpret_cast<TESForm*>(witness),
		reinterpret_cast<TESForm*>(criminal),
		static_cast<int>(crimeType),
		reinterpret_cast<TESForm*>(victim),
		static_cast<int>(detectionValue));
}

static void __fastcall Hook_AddKnow(Engine::Crime* crime, void* /*edx*/, Actor* witness)
{
	if (!g_isLoadingSave && crime && witness)
	{
		Actor*          criminal = crime->pCriminal;
		TESObjectREFR*  target   = crime->pCrimeTarget;
		UInt32          type     = crime->eCrimeType;

		SInt32 detVal = -100;
		if (criminal)
			detVal = Engine::Actor_GetDetectionValue(witness, criminal);

		DispatchWitnessedEvent(witness, criminal, type, target, detVal);
	}

	s_addKnowDetour.GetTrampoline<AddKnow_t>()(crime, witness);
}

//Actor::TrespassAlarm (retn 0Ch) ignores `this` and hardcodes the player as the trespasser,
//scanning all loaded actors itself. our own witness scan uses different criteria than the
//engine's per-actor gate, so the reported set is an approximation, not the engine's exact set.
static UInt32 __fastcall Hook_Trespass(Actor* actorThis, void* /*edx*/,
                                       TESObjectREFR* apRef, TESForm* apOwnership, UInt32 arg3)
{
	if (!g_isLoadingSave && apRef)
	{
		Actor* player = GetPlayerActor();
		if (player)
		{
			UInt32 now = GetTickCount();
			std::vector<WitnessScan::Hit> hits;
			WitnessScan::FindWitnesses(player, nullptr, 0.0f, 0, hits);
			for (const auto& hit : hits)
			{
				if (hit.actor && TrespassWitnessReady(hit.refID, now))
					DispatchWitnessedEvent(hit.actor, player,
					                       Engine::kCrimeType_Trespass, apRef, hit.detectionValue);
			}
		}
	}

	return s_trespassDetour.GetTrampoline<Trespass_t>()(actorThis, apRef, apOwnership, arg3);
}

namespace OnWitnessedHandler {
bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	//7-byte prologue
	if (!s_addKnowDetour.WriteRelJump(kAddr_AddtoActorKnowList, Hook_AddKnow, 7))
		return false;

	//9-byte prologue
	if (!s_trespassDetour.WriteRelJump(kAddr_TrespassAlarm, Hook_Trespass, 9))
	{
		s_addKnowDetour.Remove();
		return false;
	}

	return true;
}

void ClearState()
{
	memset(s_witnessIds, 0, sizeof(s_witnessIds));
	memset(s_witnessLastMs, 0, sizeof(s_witnessLastMs));
}
}
