//hooks Crime::AddtoActorKnowList (0x9EB9C0) to dispatch ITR:OnWitnessed for
//steal / pickpocket / attack / murder — engine calls it per unique witness as
//each *Alarm iterates nearby actors.
//
//trespass bypasses Crime entirely, so we also hook Actor::TrespassAlarm (0x8C0EC0)
//and run our own witness scan at entry.

#include "OnWitnessedHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/WitnessScan.h"
#include "internal/globals.h"
#include <vector>

//PlayerCharacter singleton — trespass is always player-perpetrated on the engine path
static Actor* GetPlayerActor() { return *reinterpret_cast<Actor**>(0x11DEA3C); }

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
//retn 0Ch = 3 args after this. First check inside: if (this != player) bail — so the perp
//is always the player here. This wrapper needs to return whatever the original returned
//(the function ends with `or eax, -1` then ret so it always returns -1).
static UInt32 __fastcall Hook_Trespass(Actor* actorThis, void* /*edx*/,
                                       TESObjectREFR* apRef, TESForm* apOwnership, UInt32 arg3)
{
	if (!g_isLoadingSave && actorThis && apRef)
	{
		//trespass is always player-perpetrated on this engine path (internal cmp vs PC singleton)
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

	//Crime::AddtoActorKnowList prologue at 0x9EB9C0:
	//55        push ebp
	//8B EC     mov ebp, esp
	//51        push ecx
	//89 4D FC  mov [ebp-4], ecx   <- first clean boundary >= 5 is 7 bytes
	if (!s_addKnowDetour.WriteRelJump(kAddr_AddtoActorKnowList, Hook_AddKnow, 7))
		return false;

	//Actor::TrespassAlarm prologue at 0x8C0EC0:
	//55        push ebp
	//8B EC     mov ebp, esp
	//83 EC 24  sub esp, 24h
	//89 4D E0  mov [ebp-20h], ecx  <- first clean boundary >= 5 is 9 bytes
	if (!s_trespassDetour.WriteRelJump(kAddr_TrespassAlarm, Hook_Trespass, 9))
	{
		s_addKnowDetour.Remove();
		return false;
	}

	return true;
}
}
