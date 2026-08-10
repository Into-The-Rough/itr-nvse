//four events from three hooks:
//  ITR:OnVATSEnter/OnVATSLeave - VATS::SetMode mode 4/0
//  ITR:OnKillCamStart - PlayerCharacter::StartKillcamForActor (only on 0->nonzero fKillCamTimer)
//  ITR:OnKillCamEnd   - PlayerCharacter::ForceEndKillCam

#include <cstring>

#include "OnVATSStateHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/globals.h"
#include "internal/layout/Player.h"
#include "internal/layout/VATS.h"

constexpr UInt32 kAddr_VATS_SetMode             = 0x9C6C30;
constexpr UInt32 kAddr_ForceEndKillCam          = 0x93E770;

//sole call site of PlayerCharacter::StartKillcamForActor 0x93E530, in Actor::Kill
constexpr UInt32 kAddr_StartKillcamCallSite     = 0x89F419;
constexpr UInt32 kAddr_StartKillcamForActor     = 0x93E530;

constexpr UInt8 kPrologue_VATS_SetMode[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xC4, 0xB4, 0xF1, 0x00 };
constexpr UInt8 kPrologue_ForceEndKillCam[] = { 0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC };

constexpr UInt32 kVATSMode_None     = 0;
constexpr UInt32 kVATSMode_Playback = 4;

static UInt32 g_currentKillCamTargetID = 0;

static Detours::JumpDetour s_setModeDetour;
static Detours::CallDetour s_startKillcamCall;
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
		TESForm* target = VATSGetCurrentTarget();
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

	((StartKillcamForActor_t)s_startKillcamCall.GetOverwrittenAddr())(this_, target, time, a4, a5);

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

	if (std::memcmp((void*)kAddr_VATS_SetMode, kPrologue_VATS_SetMode, sizeof(kPrologue_VATS_SetMode)) != 0)
	{
		Log("OnVATSState: VATS::SetMode prologue differs from expected, disabled");
		return false;
	}
	if (std::memcmp((void*)kAddr_ForceEndKillCam, kPrologue_ForceEndKillCam, sizeof(kPrologue_ForceEndKillCam)) != 0)
	{
		Log("OnVATSState: ForceEndKillCam prologue differs from expected, disabled");
		return false;
	}

	if (!s_setModeDetour.WriteRelJump(kAddr_VATS_SetMode, Hook_VATSSetMode, sizeof(kPrologue_VATS_SetMode)))
		return false;

	if (!s_startKillcamCall.WriteRelCall(kAddr_StartKillcamCallSite, Hook_StartKillcamForActor))
	{
		Log("OnVATSState: killcam-start call site at 0x%X is not an E8 call, disabled", kAddr_StartKillcamCallSite);
		s_setModeDetour.Remove();
		return false;
	}
	UInt32 startKillcamOriginal = s_startKillcamCall.GetOverwrittenAddr();
	Log("OnVATSState: %08X hooked, original=%08X vanilla=%08X", kAddr_StartKillcamCallSite,
		startKillcamOriginal, kAddr_StartKillcamForActor);

	if (!s_forceEndKillCamDetour.WriteRelJump(kAddr_ForceEndKillCam, Hook_ForceEndKillCam, sizeof(kPrologue_ForceEndKillCam)))
	{
		s_startKillcamCall.Remove();
		s_setModeDetour.Remove();
		return false;
	}

	return true;
}

}
