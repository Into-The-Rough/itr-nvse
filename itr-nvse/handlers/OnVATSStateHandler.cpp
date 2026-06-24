//four events from three hooks:
//  ITR:OnVATSEnter/OnVATSLeave - VATS::SetMode mode 4/0
//  ITR:OnKillCamStart - PlayerCharacter::StartKillcamForActor (only on 0->nonzero fKillCamTimer)
//  ITR:OnKillCamEnd   - PlayerCharacter::ForceEndKillCam

#include "OnVATSStateHandler.h"
#define ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/NVSEMinimal.h"
#undef ITR_NVSE_MINIMAL_SKIP_FORMTYPE
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"

constexpr UInt32 kAddr_VATS_SetMode             = 0x9C6C30;
constexpr UInt32 kAddr_StartKillcamForActor     = 0x93E530;
constexpr UInt32 kAddr_ForceEndKillCam          = 0x93E770;
constexpr UInt32 kAddr_VATSSingleton            = 0x011F2250;
constexpr UInt32 kAddr_VATSMenuCurrentTarget    = 0x011F21CC;

constexpr UInt32 kVATSMode_None     = 0;
constexpr UInt32 kVATSMode_Playback = 4;

static UInt32 g_currentKillCamTargetID = 0;

static Detours::JumpDetour s_setModeDetour;
static Detours::JumpDetour s_startKillcamDetour;
static Detours::JumpDetour s_forceEndKillCamDetour;

typedef void (__thiscall* VATS_SetMode_t)(void*, UInt32, bool);
typedef void (__thiscall* StartKillcamForActor_t)(void*, void*, float, char, int);
typedef void (__thiscall* ForceEndKillCam_t)(void*, int, bool);

static float ReadKillCamTimer() {
	auto* player = static_cast<PlayerCharacter*>(*(void**)g_thePlayerPtr);
	return PlayerCharacterGetKillCamTimer(player);
}

static void __fastcall Hook_VATSSetMode(void* this_, void* edx, UInt32 aeMode, bool abForce) {
	UInt32 oldMode = VATSCameraDataGetMode(this_);

	s_setModeDetour.GetTrampoline<VATS_SetMode_t>()(this_, aeMode, abForce);

	UInt32 newMode = VATSCameraDataGetMode(this_);
	if (oldMode == newMode || !g_eventManagerInterface) return;

	if (newMode == kVATSMode_Playback)
	{
		TESForm* target = *(TESForm**)kAddr_VATSMenuCurrentTarget;
		g_eventManagerInterface->DispatchEvent("ITR:OnVATSEnter", nullptr, target);
	}
	else if (newMode == kVATSMode_None)
	{
		//case 0 of SetMode does not zero numKills, so it still holds the count
		//from the just-finished sequence
		int numKills = static_cast<int>(VATSCameraDataGetNumKills(this_));
		g_eventManagerInterface->DispatchEvent("ITR:OnVATSLeave", nullptr, numKills);
	}
}

static void __fastcall Hook_StartKillcamForActor(void* this_, void* edx, void* target, float time, char a4, int a5) {
	float oldTimer = ReadKillCamTimer();

	s_startKillcamDetour.GetTrampoline<StartKillcamForActor_t>()(this_, target, time, a4, a5);

	float newTimer = ReadKillCamTimer();
	if (oldTimer > 0.0f || newTimer <= 0.0f || !target || !g_eventManagerInterface) return;

	auto* targetForm = static_cast<TESForm*>(target);
	g_currentKillCamTargetID = targetForm->refID;
	g_eventManagerInterface->DispatchEvent("ITR:OnKillCamStart", nullptr, targetForm);
}

static void __fastcall Hook_ForceEndKillCam(void* this_, void* edx, int a2, bool a3) {
	bool wasActive = g_currentKillCamTargetID != 0;

	s_forceEndKillCamDetour.GetTrampoline<ForceEndKillCam_t>()(this_, a2, a3);

	if (!wasActive || !g_eventManagerInterface) return;

	TESForm* target = (TESForm*)Engine::LookupFormByID(g_currentKillCamTargetID);
	g_currentKillCamTargetID = 0;
	g_eventManagerInterface->DispatchEvent("ITR:OnKillCamEnd", nullptr, target);
}

namespace OnVATSStateHandler {

void ClearState()
{
	g_currentKillCamTargetID = 0;
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	//SetMode prologue: push ebp; mov ebp,esp; push -1; push offset = 1+2+2+5 = 10
	if (!s_setModeDetour.WriteRelJump(kAddr_VATS_SetMode, Hook_VATSSetMode, 10))
		return false;

	//StartKillcamForActor prologue: same shape = 10 bytes
	if (!s_startKillcamDetour.WriteRelJump(kAddr_StartKillcamForActor, Hook_StartKillcamForActor, 10))
		return false;

	//ForceEndKillCam prologue: push ebp; mov ebp,esp; push ecx; mov [ebp-4],ecx = 1+2+1+3 = 7
	if (!s_forceEndKillCamDetour.WriteRelJump(kAddr_ForceEndKillCam, Hook_ForceEndKillCam, 7))
		return false;

	return true;
}

}
