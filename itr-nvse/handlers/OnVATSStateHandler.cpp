//fires four events from three engine hooks:
//  ITR:OnVATSEnter   -- VATS::SetMode entering mode 4 (PLAYBACK)
//  ITR:OnVATSLeave   -- VATS::SetMode entering mode 0 (NONE)
//  ITR:OnKillCamStart -- PlayerCharacter::StartKillcamForActor when fKillCamTimer
//                        actually transitions 0 -> nonzero (the function bails on
//                        most calls, only dispatch when a killcam really started)
//  ITR:OnKillCamEnd   -- PlayerCharacter::ForceEndKillCam, the chokepoint for both
//                        natural expiry (called from PlayerCharacter::Update) and
//                        forced cancellation (toggle, load)

#include "OnVATSStateHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"

constexpr UInt32 kAddr_VATS_SetMode             = 0x9C6C30;
constexpr UInt32 kAddr_StartKillcamForActor     = 0x93E530;
constexpr UInt32 kAddr_ForceEndKillCam          = 0x93E770;
constexpr UInt32 kAddr_VATSSingleton            = 0x011F2250;
constexpr UInt32 kAddr_VATSMenuCurrentTarget    = 0x011F21CC;
constexpr UInt32 kAddr_PlayerSingleton          = 0x011DEA3C;

constexpr UInt32 kVATS_eMode_Offset      = 0x08;
constexpr UInt32 kVATS_NumKills_Offset   = 0x3C;
constexpr UInt32 kPlayer_KillCamTimer_Offset = 0xE18;

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
	void* player = *(void**)kAddr_PlayerSingleton;
	if (!player) return 0.0f;
	return *(float*)((UInt8*)player + kPlayer_KillCamTimer_Offset);
}

static void __fastcall Hook_VATSSetMode(void* this_, void* edx, UInt32 aeMode, bool abForce) {
	UInt32 oldMode = *(UInt32*)((UInt8*)this_ + kVATS_eMode_Offset);

	s_setModeDetour.GetTrampoline<VATS_SetMode_t>()(this_, aeMode, abForce);

	UInt32 newMode = *(UInt32*)((UInt8*)this_ + kVATS_eMode_Offset);
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
		int numKills = *(int*)((UInt8*)this_ + kVATS_NumKills_Offset);
		g_eventManagerInterface->DispatchEvent("ITR:OnVATSLeave", nullptr, numKills);
	}
}

static void __fastcall Hook_StartKillcamForActor(void* this_, void* edx, void* target, float time, char a4, int a5) {
	float oldTimer = ReadKillCamTimer();

	s_startKillcamDetour.GetTrampoline<StartKillcamForActor_t>()(this_, target, time, a4, a5);

	float newTimer = ReadKillCamTimer();
	if (oldTimer > 0.0f || newTimer <= 0.0f || !target || !g_eventManagerInterface) return;

	//refID at TESForm+0x0C
	g_currentKillCamTargetID = *(UInt32*)((UInt8*)target + 0x0C);
	g_eventManagerInterface->DispatchEvent("ITR:OnKillCamStart", nullptr, (TESForm*)target);
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
