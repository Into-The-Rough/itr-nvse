//ITR:OnWitnessed - hook Crime::AddtoActorKnowList (0x9EB9C0), fired per witness.
//trespass bypasses Crime, so also hook Actor::TrespassAlarm (0x8C0EC0).

#include "OnWitnessedHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/WitnessScan.h"
#include "internal/globals.h"
#include "internal/GameGlobals.h"
#include <vector>

static Actor* GetPlayerActor() { return *(Actor**)g_thePlayerPtr; }

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

//Actor::TrespassAlarm signature: void __thiscall (Actor* this, TESObjectREFR* apRef, TESForm* apOwnership, UInt32);
//retn 0Ch = 3 args after this. First check inside: if (this != player) bail
static UInt32 __fastcall Hook_Trespass(Actor* actorThis, void* /*edx*/,
                                       TESObjectREFR* apRef, TESForm* apOwnership, UInt32 arg3)
{
	if (!g_isLoadingSave && actorThis && apRef)
	{
		Actor* player = GetPlayerActor();
		if (player)
		{
			std::vector<WitnessScan::Hit> hits;
			WitnessScan::FindWitnesses(player, nullptr, 0.0f, 0, hits);
			for (const auto& hit : hits)
			{
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
}
