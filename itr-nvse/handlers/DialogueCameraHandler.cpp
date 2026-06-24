#include "DialogueCameraHandler.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameObjects.h"
#include "internal/SafeWrite.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "internal/GameGlobals.h"
#include "internal/globals.h"
#include "internal/Mat3.h"
#include "internal/NiLayout.h"
#include "internal/settings.h"
#include "PerlinNoise.hpp"
#include <cmath>
#include <cstdlib>

namespace CameraHooks {
	struct NiVector3 { float x, y, z; };

	static NiVector3 g_cameraPos = {0, 0, 0};
	static Mat3 g_cameraRot;
	static bool g_overrideActive = false;

	//rotation-only override for CameraOverride (no position change)
	static Mat3 g_extRotation;
	static bool g_extRotActive = false;

	typedef void (__fastcall *SetLocalTranslate_t)(void*, void*, NiVector3*);
	typedef void (__fastcall *SetLocalRotate_t)(void*, void*, Mat3*);

	static SetLocalTranslate_t g_origTranslate1 = nullptr;
	static SetLocalTranslate_t g_origTranslate2 = nullptr;
	static SetLocalRotate_t g_origRotate1 = nullptr;
	static SetLocalRotate_t g_origRotate2 = nullptr;
	static Detours::CallDetour g_translateCall1;
	static Detours::CallDetour g_translateCall2;
	static Detours::CallDetour g_rotateCall1;
	static Detours::CallDetour g_rotateCall2;
	static bool g_hooksInstalled = false;

	void __fastcall TranslateHook1(void* node, void* edx, NiVector3* pos) {
		if (g_overrideActive) pos = &g_cameraPos;
		g_origTranslate1(node, edx, pos);
	}

	void __fastcall TranslateHook2(void* node, void* edx, NiVector3* pos) {
		if (g_overrideActive) pos = &g_cameraPos;
		g_origTranslate2(node, edx, pos);
	}

	void __fastcall RotateHook1(void* node, void* edx, Mat3* rot) {
		if (g_overrideActive) rot = &g_cameraRot;
		else if (g_extRotActive) rot = &g_extRotation;
		g_origRotate1(node, edx, rot);
	}

	void __fastcall RotateHook2(void* node, void* edx, Mat3* rot) {
		if (g_overrideActive) rot = &g_cameraRot;
		else if (g_extRotActive) rot = &g_extRotation;
		g_origRotate2(node, edx, rot);
	}

	void SetPosition(float x, float y, float z, float pitchDeg, float yawDeg) {
		g_cameraPos.x = x;
		g_cameraPos.y = y;
		g_cameraPos.z = z;

		//pitch = rotation around X axis, yaw = rotation around Z axis
		float pitchRad = pitchDeg * (3.14159265f / 180.0f);
		float yawRad = yawDeg * (3.14159265f / 180.0f);

		Mat3 rotX, rotZ;
		rotX.RotateX(pitchRad);
		rotZ.RotateZ(yawRad);
		g_cameraRot = rotZ * rotX; //yaw then pitch

		g_overrideActive = true;
	}

	void Disable() {
		g_overrideActive = false;
	}

	template<typename T>
	static bool PatchRelCall(Detours::CallDetour& detour, UInt32 src, T dst, T& orig, const char* name) {
		if (*(UInt8*)src != 0xE8) {
			Log("DialogueCameraHandler: %s patch site 0x%08X expected CALL, found 0x%02X", name, src, *(UInt8*)src);
			return false;
		}

		if (!detour.WriteRelCall(src, dst))
			return false;
		orig = reinterpret_cast<T>(detour.GetOverwrittenAddr());
		return true;
	}

	static void RollbackHooks() {
		if (g_translateCall1.Remove()) g_origTranslate1 = nullptr;
		if (g_rotateCall1.Remove()) g_origRotate1 = nullptr;
		if (g_translateCall2.Remove()) g_origTranslate2 = nullptr;
		if (g_rotateCall2.Remove()) g_origRotate2 = nullptr;
	}

	bool InstallHooks() {
		if (g_hooksInstalled)
			return true;

		//0x94AD8A - SetLocalTranslate in PlayerCharacter::HandleFlycamMovement
		//0x94AD9D - SetLocalRotate in PlayerCharacter::HandleFlycamMovement
		//0x94BDC2 - SetLocalTranslate in PlayerCharacter::UpdateCamera
		//0x94BDD5 - SetLocalRotate in PlayerCharacter::UpdateCamera
		const bool ok = PatchRelCall(g_translateCall1, 0x94AD8A, TranslateHook1, g_origTranslate1, "HandleFlycamMovement::SetLocalTranslate")
			&& PatchRelCall(g_rotateCall1, 0x94AD9D, RotateHook1, g_origRotate1, "HandleFlycamMovement::SetLocalRotate")
			&& PatchRelCall(g_translateCall2, 0x94BDC2, TranslateHook2, g_origTranslate2, "UpdateCamera::SetLocalTranslate")
			&& PatchRelCall(g_rotateCall2, 0x94BDD5, RotateHook2, g_origRotate2, "UpdateCamera::SetLocalRotate");
		if (!ok) {
			RollbackHooks();
			return false;
		}

		g_cameraRot.Identity();
		g_hooksInstalled = true;
		return true;
	}

	bool AreHooksInstalled() {
		return g_hooksInstalled;
	}

	void SetExternalRotation(const Mat3& rot) {
		g_extRotation = rot;
		g_extRotActive = true;
	}

	void ClearExternalRotation() {
		g_extRotActive = false;
	}
}

namespace DialogueCameraHandler {

//0x950110 = PlayerCharacter::SetFirstPerson(bool abFirst)
typedef void (__thiscall *SetFirstPerson_t)(void* player, bool bFirst);
static SetFirstPerson_t SetFirstPerson = (SetFirstPerson_t)0x950110;

constexpr float PI = 3.14159265358979323846f;
constexpr float kHavokScale = 0.1428571f; //1/7, game units to havok

constexpr UInt8 LAYER_STATIC = 1;
struct alignas(16) RayCastData {
	float pos0[4];       //0x00
	float pos1[4];       //0x10
	UInt8 byte20;        //0x20
	UInt8 pad21[3];
	UInt8 layerType;     //0x24
	UInt8 filterFlags;   //0x25
	UInt16 group;        //0x26
	UInt32 unk28[6];     //0x28
	float hitFraction;   //0x40 - 0.0-1.0 along ray, 1.0=no hit
	UInt32 unk44[15];    //0x44
	void* cdBody;        //0x80
	UInt32 unk84[3];     //0x84
	float vector90[4];   //0x90
	UInt32 unkA0[3];     //0xA0
	UInt8 byteAC;        //0xAC
	UInt8 padAD[3];      //0xAD
};
static_assert(sizeof(RayCastData) == 0xB0, "RayCastData size mismatch");

typedef void (__thiscall *TES_PickObject_t)(void* tes, RayCastData* rcData, bool unk); //0x458440
static TES_PickObject_t TES_PickObject = (TES_PickObject_t)0x458440;
static void** g_TES = (void**)0x11DEA10;

static float CastRay(float startX, float startY, float startZ,
					 float endX, float endY, float endZ, UInt8 layer) {
	if (!*g_TES) return 1.0f;

	RayCastData rcData = {};
	rcData.pos0[0] = startX * kHavokScale;
	rcData.pos0[1] = startY * kHavokScale;
	rcData.pos0[2] = startZ * kHavokScale;
	rcData.pos0[3] = 0.0f;
	rcData.pos1[0] = endX * kHavokScale;
	rcData.pos1[1] = endY * kHavokScale;
	rcData.pos1[2] = endZ * kHavokScale;
	rcData.pos1[3] = 0.0f;
	rcData.hitFraction = 1.0f;
	rcData.byte20 = 0;
	rcData.layerType = layer;
	rcData.filterFlags = 0;
	rcData.group = 0;
	rcData.cdBody = nullptr;

	TES_PickObject(*g_TES, &rcData, true);

	return rcData.hitFraction;
}

//<1.0 when the camera sits closer to the actor's head than minDist
static float ActorProximityFrac(float camX, float camY, float camZ,
								float lookX, float lookY, float lookZ,
								TESObjectREFR* actor, float minDist) {
	if (!actor) return 1.0f;

	float dx = camX - actor->posX;
	float dy = camY - actor->posY;
	float dz = camZ - (actor->posZ + 60.0f);
	float dist = sqrtf(dx*dx + dy*dy + dz*dz);
	if (dist >= minDist) return 1.0f;

	float lookToCamX = camX - lookX;
	float lookToCamY = camY - lookY;
	float lookToCamZ = camZ - lookZ;
	float totalDist = sqrtf(lookToCamX*lookToCamX + lookToCamY*lookToCamY + lookToCamZ*lookToCamZ);
	if (totalDist <= 1.0f) return 1.0f;

	return (totalDist - (minDist - dist) * 1.5f) / totalDist;
}

//returns safe fraction from look target (1.0=ok, <1.0=pull closer)
static float CheckCameraClip(float camX, float camY, float camZ,
							 float lookX, float lookY, float lookZ,
							 TESObjectREFR* npc, TESObjectREFR* player) {
	float safeFrac = 1.0f;

	float frac = ActorProximityFrac(camX, camY, camZ, lookX, lookY, lookZ, npc, 40.0f);
	if (frac < safeFrac) safeFrac = frac;
	frac = ActorProximityFrac(camX, camY, camZ, lookX, lookY, lookZ, player, 35.0f);
	if (frac < safeFrac) safeFrac = frac;

	//raycast from look target to camera, offset start past actor collision
	float rayDirX = camX - lookX;
	float rayDirY = camY - lookY;
	float rayDirZ = camZ - lookZ;
	float rayLen = sqrtf(rayDirX*rayDirX + rayDirY*rayDirY + rayDirZ*rayDirZ);
	if (rayLen > 60.0f) {
		float offsetDist = 50.0f;
		float startX = lookX + (rayDirX / rayLen) * offsetDist;
		float startY = lookY + (rayDirY / rayLen) * offsetDist;
		float startZ = lookZ + (rayDirZ / rayLen) * offsetDist;

		float staticHit = CastRay(startX, startY, startZ, camX, camY, camZ, LAYER_STATIC);
		if (staticHit < 0.95f) {
			float remainingLen = rayLen - offsetDist;
			float hitDist = offsetDist + staticHit * remainingLen;
			float fullFrac = (hitDist / rayLen) * 0.85f;
			if (fullFrac < safeFrac) {
				safeFrac = fullFrac;
			}
		}
	}

	if (safeFrac < 0.15f) safeFrac = 0.15f;

	return safeFrac;
}

enum CameraAngle {
	kAngle_Vanilla,
	kAngle_OverShoulder,
	kAngle_NPCCloseup,
	kAngle_TwoShot,
	kAngle_NPCFace,
	kAngle_LowAngle,
	kAngle_HighAngle,
	kAngle_PlayerFace,
	kAngle_WideShot,
	kAngle_NPCProfile,
	kAngle_PlayerProfile,
	kAngle_Overhead,
	kAngle_Count
};

enum CameraAngleMode {
	kAngleMode_Cycle,
	kAngleMode_Fixed,
	kAngleMode_Random,
	kAngleMode_Manual
};

struct InterfaceManager {
	UInt8 pad[0xFC];
	TESObjectREFR* crosshairRef;  //0xFC

	static InterfaceManager* GetSingleton() {
		return *(InterfaceManager**)0x11D8A80;
	}
};

struct DialogueMenu {
	UInt8 pad[0x48];
	void* currentInfo;  //0x48

	static DialogueMenu* GetSingleton() {
		return *(DialogueMenu**)0x11D9510;
	}
};

static UInt8* g_MenuVisibilityArray = (UInt8*)0x011F308F;
static constexpr UInt32 kMenuType_Dialogue = 1009;

static bool g_inDialogue = false;
static TESObjectREFR* g_dialogueTarget = nullptr;
static UInt32 g_dialogueTargetID = 0;
static CameraAngle g_currentAngle = kAngle_Vanilla;
static CameraAngleMode g_angleMode = kAngleMode_Cycle;
static CameraAngle g_fixedAngle = kAngle_Vanilla;
static bool g_cameraActive = false;
static bool g_wasFirstPerson = false;
static int g_dialogueLineCount = 0;
static UInt32 g_lastTopicInfoID = 0;

static float g_baseCamX = 0, g_baseCamY = 0, g_baseCamZ = 0;
static float g_baseLookX = 0, g_baseLookY = 0, g_baseLookZ = 0;
static bool g_hasPrevShot = false;
static double g_noiseTime = 0;

static float g_transFromCamX = 0, g_transFromCamY = 0, g_transFromCamZ = 0;
static float g_transFromLookX = 0, g_transFromLookY = 0, g_transFromLookZ = 0;
static float g_transToCamX = 0, g_transToCamY = 0, g_transToCamZ = 0;
static float g_transToLookX = 0, g_transToLookY = 0, g_transToLookZ = 0;
static float g_transProgress = 1.0f; //1.0 = no transition active
static float g_transSpeed = 1.5f; //progress per second (~0.7s transition)
static bool g_patchesInstalled = false;
float g_dollySpeed = 0.0f;
float g_dollyMaxDist = 0.0f;
int g_dollyRunOnce = 1;
static float g_dollyProgress = 0.0f;
static bool g_dollyFirstDone = false;
float g_shakeAmplitude = -1.0f; //-1 = use default
static bool g_rngSeeded = false;
static bool g_playerMovementSettled = false;

static const siv::PerlinNoise g_perlinPitch{ 4 };
static const siv::PerlinNoise g_perlinYaw{ 5 };

//stateful - call exactly once per frame, from Update only
static float FrameDelta()
{
	static LARGE_INTEGER s_freq = {};
	static LARGE_INTEGER s_last = {};
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!s_freq.QuadPart)
	{
		QueryPerformanceFrequency(&s_freq);
		s_last = now;
		return 1.0f / 60.0f;
	}
	float dt = (float)((double)(now.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart);
	s_last = now;
	if (dt > 0.1f) dt = 0.1f;   //clamp hitches so shots don't teleport
	if (dt < 0.0f) dt = 0.0f;
	return dt;
}

static int* g_pDialogueCamera = &Settings::bDialogueCamera;
static const UInt32 kAddr_bShouldRestore1stPerson = 0x11F21D0;
static Detours::CallDetour s_show1stPersonFocusCall;
static Detours::CallDetour s_show1stPersonDialogCall;
static Detours::CallDetour s_pickAnimationsCall;
static Detours::CallDetour s_setFirstPersonCall;
typedef void(__thiscall* Show1stPerson_t)(void*, bool);
typedef void(__thiscall* PickAnimations_t)(void*, float, float);

struct DialogueVec3 { float x, y, z; };

static void ClearPlayerDialogueMovement(PlayerCharacter* player)
{
	if (!player) return;

	void* mover = player->actorMover;
	if (!mover) return;

	DialogueVec3 zero = {};
	ThisCall<void>(0x9EA570, mover, &zero); //PlayerMover::SetSpeedVector
	ThisCall<void>(0x9EA3B0, mover, 0x3F); //PlayerMover::ClearMovementFlag_
}

//sites 1&2: call wrappers for Show1stPerson (0x951A10)
//when dialogue camera active, skip the force-third-person call
static void Hook_Show1stPerson(Detours::CallDetour& detour, void* player, bool show) {
	if (!Settings::bDialogueCamera)
	{
		auto original = reinterpret_cast<Show1stPerson_t>(detour.GetOverwrittenAddr());
		if (original)
			original(player, show);
	}
}

static void __fastcall Hook_Show1stPerson_Focus(void* player, void*, bool show) {
	Hook_Show1stPerson(s_show1stPersonFocusCall, player, show);
}

static void __fastcall Hook_Show1stPerson_Dialog(void* player, void*, bool show) {
	Hook_Show1stPerson(s_show1stPersonDialogCall, player, show);
}

//site 3: FocusOnActor force-third-person branch (0x953ABF)
//original: push 1; mov ecx, [ebp-0x9C] (8 bytes)
static const UInt32 kSite3_Return = 0x953AC7;
static const UInt32 kSite3_Skip = 0x953AF5;
__declspec(naked) void Hook_ForceThirdPerson_Branch1() {
	__asm {
		mov eax, g_pDialogueCamera
		cmp dword ptr [eax], 0
		jnz skip
		push 1
		mov ecx, [ebp-0x9C]
		jmp kSite3_Return
	skip:
		jmp kSite3_Skip
	}
}

//site 4: DoIdle save/restore first-person state (0x762E55)
//original: mov byte ptr [0x11F21D0], 1 (7 bytes)
static const UInt32 kSite4_Return = 0x762E5C;
static const UInt32 kSite4_Skip = 0x762E72;
__declspec(naked) void Hook_ForceThirdPerson_Branch2() {
	__asm {
		mov eax, g_pDialogueCamera
		cmp dword ptr [eax], 0
		jnz skip
		mov eax, kAddr_bShouldRestore1stPerson
		mov byte ptr [eax], 1
		jmp kSite4_Return
	skip:
		jmp kSite4_Skip
	}
}

//site 5: FocusOnActor dialogue zoom conditional (0x9533BE)
//original: jz 0x953562 (6 bytes, 0F 84 9E 01 00 00)
//when dialogue camera active, always skip zoom
static const UInt32 kSite5_Zoom = 0x9533C4;
static const UInt32 kSite5_NoZoom = 0x953562;
__declspec(naked) void Hook_DisableDialogueZoom() {
	__asm {
		mov eax, g_pDialogueCamera
		cmp dword ptr [eax], 0
		jnz skip_zoom
		cmp dword ptr [ebp-0x44], 0
		jz skip_zoom
		jmp kSite5_Zoom
	skip_zoom:
		jmp kSite5_NoZoom
	}
}

//site 6: FocusOnActor fallback SetFOV (0x953BB4) - fires every frame when site 5
//skips the zoom block, skip it when dialogue camera active. patched site = 8 bytes.
static const UInt32 kSite6_Continue = 0x953BBC;
static const UInt32 kSite6_Skip = 0x953BFB;
__declspec(naked) void Hook_SkipFallbackFOV() {
	__asm {
		mov eax, g_pDialogueCamera
		cmp dword ptr [eax], 0
		jnz skip_fov
		movzx eax, byte ptr [ebp-0x15]
		test eax, eax
		jnz skip_fov
		jmp kSite6_Continue
	skip_fov:
		jmp kSite6_Skip
	}
}

//site 7: suppress per-frame PickAnimations in FocusOnActor's heading block.
//Allow one clean reselect after dialogue starts so movement entered from a run
//doesn't keep a stale locomotion group on the visible 3rd person body.
static void __fastcall Hook_SkipPickAnimations(void* actor, void*, float a1, float a2) {
	auto original = reinterpret_cast<PickAnimations_t>(s_pickAnimationsCall.GetOverwrittenAddr());
	if (!original)
		return;

	if (!Settings::bDialogueCamera)
	{
		original(actor, a1, a2);
		return;
	}

	if (!g_playerMovementSettled && (g_inDialogue || g_MenuVisibilityArray[kMenuType_Dialogue]))
	{
		ClearPlayerDialogueMovement(static_cast<PlayerCharacter*>(actor));
		original(actor, a1, a2);
		g_playerMovementSettled = true;
	}
}

//site 8: SetFirstPerson(true) in heading block (0x953AC7)
//the heading block forces 1st person every frame, which prevents the
//3rd person model from being updated with the new rotation.
static void __fastcall Hook_SkipSetFirstPerson(void* player, void*, bool bFirst) {
	if (!Settings::bDialogueCamera)
	{
		auto original = reinterpret_cast<SetFirstPerson_t>(s_setFirstPersonCall.GetOverwrittenAddr());
		if (original)
			original(player, bFirst);
	}
}

template <typename T>
static bool InstallCallSite(Detours::CallDetour& detour, UInt32 addr, T hook, const char* name)
{
	if (*(UInt8*)addr != 0xE8) {
		Log("DialogueCameraHandler: %s expected CALL at 0x%08X, found 0x%02X", name, addr, *(UInt8*)addr);
		return false;
	}
	return detour.WriteRelCall(addr, hook);
}

static bool InstallDialoguePatches() {
	if (g_patchesInstalled)
		return true;

	//verify every site before writing anything - partial install would suppress
	//vanilla first-person dialogue with no replacement camera
	struct SiteCheck { UInt32 addr; UInt8 expect; const char* name; };
	static const SiteCheck kSites[] = {
		{ 0x953124, 0xE8, "site 1 (FocusOnActor::Show1stPerson)" },
		{ 0x761DEF, 0xE8, "site 2 (DialogMenu::Create::Show1stPerson)" },
		{ 0x953ABF, 0x6A, "site 3 (force-third-person branch 1)" },
		{ 0x762E55, 0xC6, "site 4 (DoIdle save first-person)" },
		{ 0x9533BE, 0x0F, "site 5 (dialogue zoom conditional)" },
		{ 0x953BB4, 0x0F, "site 6 (fallback SetFOV)" },
		{ 0x953B2F, 0xE8, "site 7 (PickAnimations)" },
		{ 0x953AC7, 0xE8, "site 8 (SetFirstPerson)" },
	};
	for (const auto& s : kSites)
	{
		if (*(UInt8*)s.addr != s.expect)
		{
			Log("DialogueCameraHandler: %s expected 0x%02X at 0x%08X, found 0x%02X - dialogue camera disabled", s.name, s.expect, s.addr, *(UInt8*)s.addr);
			return false;
		}
	}

	InstallCallSite(s_show1stPersonFocusCall, 0x953124, Hook_Show1stPerson_Focus, "site 1 (FocusOnActor::Show1stPerson)");
	InstallCallSite(s_show1stPersonDialogCall, 0x761DEF, Hook_Show1stPerson_Dialog, "site 2 (DialogMenu::Create::Show1stPerson)");

	//site 3: FocusOnActor force-third-person branch (8 bytes: push 1 + mov ecx)
	SafeWrite::WriteRelJump(0x953ABF, (UInt32)Hook_ForceThirdPerson_Branch1);
	SafeWrite::WriteNop(0x953AC4, 3); //pad remainder of 8-byte overwrite

	//site 4: DoIdle save first-person state (7 bytes: mov byte ptr)
	SafeWrite::WriteRelJump(0x762E55, (UInt32)Hook_ForceThirdPerson_Branch2);
	SafeWrite::WriteNop(0x762E5A, 2); //pad remainder of 7-byte overwrite

	//site 5: FocusOnActor dialogue zoom conditional (6 bytes: jz near)
	SafeWrite::WriteRelJump(0x9533BE, (UInt32)Hook_DisableDialogueZoom);
	SafeWrite::Write8(0x9533C3, 0x90); //pad 6th byte

	//site 6: FocusOnActor fallback SetFOV (8 bytes: movzx+test+jnz)
	SafeWrite::WriteRelJump(0x953BB4, (UInt32)Hook_SkipFallbackFOV);
	SafeWrite::WriteNop(0x953BB9, 3); //pad remainder of 8-byte overwrite

	InstallCallSite(s_pickAnimationsCall, 0x953B2F, Hook_SkipPickAnimations, "site 7 (PickAnimations)");
	InstallCallSite(s_setFirstPersonCall, 0x953AC7, Hook_SkipSetFirstPerson, "site 8 (SetFirstPerson)");

	g_patchesInstalled = true;
	return true;
}

typedef void* (__cdecl* GetObjectByName_t)(void* rootNode, const char* name);
static GetObjectByName_t GetObjectByName = (GetObjectByName_t)0x4AAE30;

static bool GetHeadPos(TESObjectREFR* ref, float& outX, float& outY, float& outZ)
{
	if (!ref) return false;
	void* rootNode = TESObjectREFRGetNiNodeRaw(ref);
	if (!rootNode) return false;

	//try head bone, fall back to neck
	void* bone = GetObjectByName(rootNode, "Bip01 Head");
	if (!bone) bone = GetObjectByName(rootNode, "Bip01 Neck1");
	if (!bone) return false;

	float* worldPos = NiAVObjectAsView(bone)->world.translate;
	outX = worldPos[0];
	outY = worldPos[1];
	outZ = worldPos[2];
	return true;
}

static void ApplyCameraAngle(CameraAngle angle);

static bool IsValidCameraAngle(int angle)
{
	return angle >= 0 && angle < kAngle_Count;
}

static const char* GetCameraAngleName(CameraAngle angle)
{
	switch (angle) {
		case kAngle_Vanilla: return "Vanilla";
		case kAngle_OverShoulder: return "OverShoulder";
		case kAngle_NPCCloseup: return "NPCCloseup";
		case kAngle_TwoShot: return "TwoShot";
		case kAngle_NPCFace: return "NPCFace";
		case kAngle_LowAngle: return "LowAngle";
		case kAngle_HighAngle: return "HighAngle";
		case kAngle_PlayerFace: return "PlayerFace";
		case kAngle_WideShot: return "WideShot";
		case kAngle_NPCProfile: return "NPCProfile";
		case kAngle_PlayerProfile: return "PlayerProfile";
		case kAngle_Overhead: return "Overhead";
		default: return "Unknown";
	}
}

static const char* GetCameraModeName(CameraAngleMode mode)
{
	switch (mode) {
		case kAngleMode_Cycle: return "Cycle";
		case kAngleMode_Fixed: return "Fixed";
		case kAngleMode_Random: return "Random";
		case kAngleMode_Manual: return "Manual";
		default: return "Unknown";
	}
}

static CameraAngle GetRandomCameraAngle(CameraAngle exclude = kAngle_Count)
{
	if (!g_rngSeeded) {
		srand(GetTickCount());
		g_rngSeeded = true;
	}

	int choice = rand() % kAngle_Count;
	if (exclude < kAngle_Count && kAngle_Count > 1) {
		for (int i = 0; i < 4 && choice == exclude; i++)
			choice = rand() % kAngle_Count;
		if (choice == exclude)
			choice = (choice + 1) % kAngle_Count;
	}

	return (CameraAngle)choice;
}

static CameraAngle SelectDialogueAngle(bool dialogueStart)
{
	switch (g_angleMode) {
		case kAngleMode_Cycle:
			return dialogueStart ? kAngle_Vanilla : (CameraAngle)(g_dialogueLineCount % kAngle_Count);
		case kAngleMode_Fixed:
			return g_fixedAngle;
		case kAngleMode_Random:
			return GetRandomCameraAngle(dialogueStart ? kAngle_Count : g_currentAngle);
		case kAngleMode_Manual:
			return g_currentAngle;
		default:
			return kAngle_Vanilla;
	}
}

static void ApplySelectedAngle(CameraAngle angle, bool forceDollyReset = false)
{
	g_currentAngle = angle;
	if (forceDollyReset || !g_dollyRunOnce || !g_dollyFirstDone)
		g_dollyProgress = 0.0f;
	ApplyCameraAngle(g_currentAngle);
}

static void ApplyCameraAngle(CameraAngle angle) {
	PlayerCharacter* player = *g_thePlayerPtr;
	if (!player || !g_dialogueTarget) return;

	//a result script can disable or delete the speaker mid-dialogue, revalidate before touching it
	if (Engine::LookupFormByID(g_dialogueTargetID) != g_dialogueTarget) return;
	if (!g_dialogueTarget->renderState) return;

	//use actual head positions when available, fall back to estimated
	float px, py, pz;
	if (!GetHeadPos((TESObjectREFR*)player, px, py, pz)) {
		px = player->posX;
		py = player->posY;
		pz = player->posZ + 60.0f;
	}

	float nx, ny, nz;
	if (!GetHeadPos(g_dialogueTarget, nx, ny, nz)) {
		nx = g_dialogueTarget->posX;
		ny = g_dialogueTarget->posY;
		nz = g_dialogueTarget->posZ + 60.0f;
	}

	float dx = nx - px;
	float dy = ny - py;
	float dist = sqrtf(dx*dx + dy*dy);
	if (dist < 1.0f) dist = 1.0f;

	float dirX = dx / dist;
	float dirY = dy / dist;

	float perpX = -dirY;
	float perpY = dirX;

	float midX = (px + nx) / 2.0f;
	float midY = (py + ny) / 2.0f;
	float midZ = (pz + nz) / 2.0f;

	//scale offsets by distance - designed for ~100 unit separation
	float s = dist / 100.0f;
	if (s < 0.5f) s = 0.5f;
	if (s > 2.0f) s = 2.0f;

	struct CamCandidate {
		float x, y, z;
		float lookX, lookY, lookZ;
	};
	CamCandidate candidates[2];

	switch (angle) {
		case kAngle_Vanilla:
			candidates[0] = { px + dirX * 5.0f, py + dirY * 5.0f, pz + 50.0f*s, nx, ny, nz };
			candidates[1] = candidates[0];
			break;
		case kAngle_OverShoulder:
			candidates[0] = { px - dirX*70*s + perpX*50*s, py - dirY*70*s + perpY*50*s, pz + 20.0f, nx, ny, nz };
			candidates[1] = { px - dirX*70*s - perpX*50*s, py - dirY*70*s - perpY*50*s, pz + 20.0f, nx, ny, nz };
			break;
		case kAngle_NPCCloseup:
			candidates[0] = { nx + dirX*40*s + perpX*80*s, ny + dirY*40*s + perpY*80*s, nz, px, py, pz };
			candidates[1] = { nx + dirX*40*s - perpX*80*s, ny + dirY*40*s - perpY*80*s, nz, px, py, pz };
			break;
		case kAngle_TwoShot:
			candidates[0] = { midX + perpX*120*s, midY + perpY*120*s, midZ + 20.0f, midX, midY, midZ };
			candidates[1] = { midX - perpX*120*s, midY - perpY*120*s, midZ + 20.0f, midX, midY, midZ };
			break;
		case kAngle_NPCFace:
			candidates[0] = { nx - dirX*60*s + perpX*20*s, ny - dirY*60*s + perpY*20*s, nz + 5.0f, nx, ny, nz };
			candidates[1] = { nx - dirX*60*s - perpX*20*s, ny - dirY*60*s - perpY*20*s, nz + 5.0f, nx, ny, nz };
			break;
		case kAngle_LowAngle:
			candidates[0] = { nx - dirX*80*s + perpX*30*s, ny - dirY*80*s + perpY*30*s, nz - 30.0f, nx, ny, nz };
			candidates[1] = { nx - dirX*80*s - perpX*30*s, ny - dirY*80*s - perpY*30*s, nz - 30.0f, nx, ny, nz };
			break;
		case kAngle_HighAngle:
			candidates[0] = { nx - dirX*60*s + perpX*30*s, ny - dirY*60*s + perpY*30*s, nz + 80.0f*s, nx, ny, nz };
			candidates[1] = { nx - dirX*60*s - perpX*30*s, ny - dirY*60*s - perpY*30*s, nz + 80.0f*s, nx, ny, nz };
			break;
		case kAngle_PlayerFace:
			candidates[0] = { px + perpX*80*s, py + perpY*80*s, pz, nx, ny, nz };
			candidates[1] = { px - perpX*80*s, py - perpY*80*s, pz, nx, ny, nz };
			break;
		case kAngle_WideShot:
			candidates[0] = { midX + perpX*200*s, midY + perpY*200*s, midZ + 60.0f*s, midX, midY, midZ };
			candidates[1] = { midX - perpX*200*s, midY - perpY*200*s, midZ + 60.0f*s, midX, midY, midZ };
			break;
		case kAngle_NPCProfile:
			candidates[0] = { nx + perpX*90*s, ny + perpY*90*s, nz, nx, ny, nz };
			candidates[1] = { nx - perpX*90*s, ny - perpY*90*s, nz, nx, ny, nz };
			break;
		case kAngle_PlayerProfile:
			candidates[0] = { px + perpX*90*s, py + perpY*90*s, pz, px, py, pz };
			candidates[1] = { px - perpX*90*s, py - perpY*90*s, pz, px, py, pz };
			break;
		case kAngle_Overhead:
			candidates[0] = { midX, midY, midZ + 150.0f*s, midX, midY, midZ };
			candidates[1] = candidates[0];
			break;
		default:
			return;
	}

	int chosen = 0;
	float bestClipFrac = 0.0f;

	for (int i = 0; i < 2; i++) {
		float clipFrac = CheckCameraClip(candidates[i].x, candidates[i].y, candidates[i].z,
										 candidates[i].lookX, candidates[i].lookY, candidates[i].lookZ,
										 g_dialogueTarget, player);

		if (clipFrac > bestClipFrac) {
			bestClipFrac = clipFrac;
			chosen = i;
		}

		if (clipFrac >= 0.95f) {
			break;
		}
	}

	float camX = candidates[chosen].x;
	float camY = candidates[chosen].y;
	float camZ = candidates[chosen].z;
	float lookX = candidates[chosen].lookX;
	float lookY = candidates[chosen].lookY;
	float lookZ = candidates[chosen].lookZ;

	float finalClipFrac = CheckCameraClip(camX, camY, camZ, lookX, lookY, lookZ, g_dialogueTarget, player);
	if (finalClipFrac < 0.95f) {
		camX = lookX + (camX - lookX) * finalClipFrac;
		camY = lookY + (camY - lookY) * finalClipFrac;
		camZ = lookZ + (camZ - lookZ) * finalClipFrac;
	}

	bool isFirstAngle = !g_hasPrevShot;
	g_hasPrevShot = true;

	if (!Settings::bSmoothCameraAngleInterp || isFirstAngle)
	{
		g_baseCamX = camX;
		g_baseCamY = camY;
		g_baseCamZ = camZ;
		g_baseLookX = lookX;
		g_baseLookY = lookY;
		g_baseLookZ = lookZ;

		float toDirX = lookX - camX;
		float toDirY = lookY - camY;
		float toDirZ = lookZ - camZ;
		float horizDist = sqrtf(toDirX*toDirX + toDirY*toDirY);
		float yaw = atan2f(toDirX, toDirY) * (180.0f / PI);
		float pitch = -atan2f(toDirZ, horizDist) * (180.0f / PI);
		CameraHooks::SetPosition(camX, camY, camZ, pitch, yaw);
	}
	else
	{
		g_transFromCamX = g_baseCamX;
		g_transFromCamY = g_baseCamY;
		g_transFromCamZ = g_baseCamZ;
		g_transFromLookX = g_baseLookX;
		g_transFromLookY = g_baseLookY;
		g_transFromLookZ = g_baseLookZ;
		g_transToCamX = camX;
		g_transToCamY = camY;
		g_transToCamZ = camZ;
		g_transToLookX = lookX;
		g_transToLookY = lookY;
		g_transToLookZ = lookZ;
		g_transProgress = 0.0f;
	}
}

static void ApplyCameraNoise(float dt) {
	float rotAmp = (g_shakeAmplitude >= 0.0f) ? g_shakeAmplitude : (float)Settings::iShakeAmplitude;
	if (rotAmp > 15.0f) rotAmp = 15.0f;

	//advance transition
	if (g_transProgress < 1.0f)
	{
		g_transProgress += g_transSpeed * dt;
		if (g_transProgress > 1.0f) g_transProgress = 1.0f;

		float t = g_transProgress * g_transProgress * (3.0f - 2.0f * g_transProgress);
		g_baseCamX = g_transFromCamX + (g_transToCamX - g_transFromCamX) * t;
		g_baseCamY = g_transFromCamY + (g_transToCamY - g_transFromCamY) * t;
		g_baseCamZ = g_transFromCamZ + (g_transToCamZ - g_transFromCamZ) * t;
		g_baseLookX = g_transFromLookX + (g_transToLookX - g_transFromLookX) * t;
		g_baseLookY = g_transFromLookY + (g_transToLookY - g_transFromLookY) * t;
		g_baseLookZ = g_transFromLookZ + (g_transToLookZ - g_transFromLookZ) * t;
	}

	float toDirX = g_baseLookX - g_baseCamX;
	float toDirY = g_baseLookY - g_baseCamY;
	float toDirZ = g_baseLookZ - g_baseCamZ;

	float camX = g_baseCamX;
	float camY = g_baseCamY;
	float camZ = g_baseCamZ;

	//dolly: smoothstep zoom toward look target per angle, holds at max
	if (g_dollyMaxDist != 0.0f)
	{
		if (g_dollyProgress < 1.0f)
		{
			g_dollyProgress += g_dollySpeed * 0.06f * dt;
			if (g_dollyProgress > 1.0f)
			{
				g_dollyProgress = 1.0f;
				g_dollyFirstDone = true;
			}
		}

		float toDist = sqrtf(toDirX*toDirX + toDirY*toDirY + toDirZ*toDirZ);
		if (toDist > 1.0f)
		{
			float t = g_dollyProgress * g_dollyProgress * (3.0f - 2.0f * g_dollyProgress);
			float dolly = t * g_dollyMaxDist;
			camX += (toDirX / toDist) * dolly;
			camY += (toDirY / toDist) * dolly;
			camZ += (toDirZ / toDist) * dolly;
		}
	}

	float horizDist = sqrtf(toDirX*toDirX + toDirY*toDirY);
	float yaw = atan2f(toDirX, toDirY) * (180.0f / PI);
	float pitch = -atan2f(toDirZ, horizDist) * (180.0f / PI);

	yaw += (float)g_perlinYaw.octave1D(g_noiseTime, 3, 0.5) * rotAmp;
	pitch += (float)g_perlinPitch.octave1D(g_noiseTime, 3, 0.5) * rotAmp * 0.5f;

	CameraHooks::SetPosition(camX, camY, camZ, pitch, yaw);
}

static void OnDialogueStart() {

	InterfaceManager* intfc = InterfaceManager::GetSingleton();
	if (!intfc) return;

	g_dialogueTarget = intfc->crosshairRef;
	if (!g_dialogueTarget) return;

	//0x3B=Character, 0x3C=Creature
	UInt8 typeID = g_dialogueTarget->typeID;
	if (typeID != 0x3B && typeID != 0x3C) {
		g_dialogueTarget = nullptr;
		return;
	}
	g_dialogueTargetID = g_dialogueTarget->refID;

	PlayerCharacter* player = *g_thePlayerPtr;
	if (!player) return;

	g_wasFirstPerson = !player->bThirdPerson;
	if (g_wasFirstPerson) {
		SetFirstPerson(player, false);
	}
	ClearPlayerDialogueMovement(player);

	g_dialogueLineCount = 0;
	g_lastTopicInfoID = 0;
	g_dollyProgress = 0.0f;
	g_dollyFirstDone = false;
	g_transProgress = 1.0f;
	g_baseCamX = g_baseCamY = g_baseCamZ = 0;
	g_baseLookX = g_baseLookY = g_baseLookZ = 0;
	g_hasPrevShot = false;
	g_cameraActive = true;

	g_currentAngle = SelectDialogueAngle(true);
	ApplySelectedAngle(g_currentAngle, true);
}

static void OnDialogueEnd() {
	if (g_cameraActive) {
		CameraHooks::Disable();
		if (g_wasFirstPerson) {
			PlayerCharacter* player = *g_thePlayerPtr;
			if (player) {
				SetFirstPerson(player, true);
			}
		}
		g_cameraActive = false;
	}
	g_dialogueTarget = nullptr;
	g_dialogueTargetID = 0;
	g_playerMovementSettled = false;
}

void Update() {
	if (!g_patchesInstalled || !CameraHooks::AreHooksInstalled())
		return;

	bool dialogueMenuVisible = g_MenuVisibilityArray[kMenuType_Dialogue] != 0;

	if (dialogueMenuVisible && !g_inDialogue) {
		g_inDialogue = true;
		OnDialogueStart();
	}
	else if (!dialogueMenuVisible && g_inDialogue) {
		g_inDialogue = false;
		OnDialogueEnd();
	}

	if (!g_cameraActive || !g_dialogueTarget) return;

	DialogueMenu* dlgMenu = DialogueMenu::GetSingleton();
	if (dlgMenu) {
		UInt32 currentInfoAddr = (UInt32)dlgMenu->currentInfo;
		if (currentInfoAddr != 0 && currentInfoAddr != g_lastTopicInfoID) {
			g_lastTopicInfoID = currentInfoAddr;
			g_dialogueLineCount++;
			if (g_angleMode != kAngleMode_Manual)
				ApplySelectedAngle(SelectDialogueAngle(false));
		}
	}

	//apply player heading to 3rd person NiNode - the heading block updates
	//rotZ correctly but b3rdPerson toggling prevents the 3D from following
	PlayerCharacter* player = *g_thePlayerPtr;
	if (player) {
		void* node3rd = ThisCall<void*>(0x950BB0, player, 0); //GetNode(3rdPerson)
		if (node3rd) {
			Mat3 rot;
			rot.RotateZ(player->rotZ);
			ThisCall<void>(0x43FA80, node3rd, &rot); //NiAVObject::SetLocalRotate
		}
	}

	const float dt = FrameDelta();
	g_noiseTime += 0.3 * dt;
	ApplyCameraNoise(dt);
}

bool InstallCameraHooks() {
	if (!CameraHooks::InstallHooks())
		return false;
	if (!InstallDialoguePatches()) {
		//no dialogue patch was written, camera hooks stay installed but inert
		//so dialogue behaves fully vanilla
		CameraHooks::Disable();
		return false;
	}
	return true;
}

}

namespace DialogueCameraHandler {
bool Init(void*) {
	return true;
}

void SetEnabled(bool enabled) {
	Settings::bDialogueCamera = enabled ? 1 : 0;
	if (!enabled) {
		OnDialogueEnd();
		g_inDialogue = false;
	}
}

bool IsEnabled() {
	return Settings::bDialogueCamera != 0;
}

bool SetAngleMode(int mode) {
	if (mode < 0 || mode > kAngleMode_Manual)
		return false;

	g_angleMode = (CameraAngleMode)mode;
	if (g_cameraActive && g_dialogueTarget && g_angleMode == kAngleMode_Fixed)
		ApplySelectedAngle(g_fixedAngle, true);
	return true;
}

bool SetFixedAngle(int angle) {
	if (!IsValidCameraAngle(angle))
		return false;

	g_fixedAngle = (CameraAngle)angle;
	if (g_cameraActive && g_dialogueTarget && g_angleMode == kAngleMode_Fixed)
		ApplySelectedAngle(g_fixedAngle, true);
	return true;
}

int SetCurrentAngle(int angle) {
	CameraAngle chosenAngle;
	if (angle < 0)
		chosenAngle = GetRandomCameraAngle(g_currentAngle);
	else {
		if (!IsValidCameraAngle(angle))
			return -1;
		chosenAngle = (CameraAngle)angle;
	}

	if (g_angleMode == kAngleMode_Fixed)
		g_fixedAngle = chosenAngle;

	if (g_cameraActive && g_dialogueTarget)
		ApplySelectedAngle(chosenAngle, true);
	else
		g_currentAngle = chosenAngle;

	return (int)chosenAngle;
}

void SetDolly(float speed, float maxDist, int runOnce) {
	g_dollySpeed = speed;
	g_dollyMaxDist = maxDist;
	g_dollyRunOnce = runOnce;
}

void SetShakeAmplitude(float amplitude) {
	g_shakeAmplitude = amplitude;
}

DebugState GetDebugState() {
	return {
		IsEnabled(),
		(int)g_angleMode,
		(int)g_fixedAngle,
		(int)g_currentAngle,
		g_dollySpeed,
		g_dollyMaxDist,
		g_dollyRunOnce,
		g_shakeAmplitude
	};
}

void RestoreDebugState(const DebugState& state) {
	SetEnabled(state.enabled);
	g_currentAngle = IsValidCameraAngle(state.currentAngle) ? (CameraAngle)state.currentAngle : kAngle_Vanilla;
	SetAngleMode(state.angleMode);
	SetFixedAngle(state.fixedAngle);
	SetDolly(state.dollySpeed, state.dollyMaxDist, state.dollyRunOnce);
	SetShakeAmplitude(state.shakeAmplitude);
}

void SetExternalRotation(const Mat3& rot) {
	CameraHooks::SetExternalRotation(rot);
}

void ClearExternalRotation() {
	CameraHooks::ClearExternalRotation();
}
}

static ParamInfo kParams_SetDialogueCameraEnabled[1] = {
	{"enable", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetDialogueCameraEnabled, "Enable or disable dialogue camera at runtime", 0, 1, kParams_SetDialogueCameraEnabled);

bool Cmd_SetDialogueCameraEnabled_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 enable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &enable))
		return true;

	DialogueCameraHandler::SetEnabled(enable != 0);
	*result = 1;

	if (IsConsoleMode())
		Console_Print("DialogueCamera >> %s", enable ? "enabled" : "disabled");
	return true;
}

static ParamInfo kParams_SetDialogueCameraMode[1] = {
	{"mode", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetDialogueCameraMode, "Set auto angle mode: 0=cycle, 1=fixed, 2=random, 3=manual", 0, 1, kParams_SetDialogueCameraMode);

bool Cmd_SetDialogueCameraMode_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 mode = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &mode))
		return true;
	if (!DialogueCameraHandler::SetAngleMode((int)mode))
		return true;

	*result = 1;

	if (IsConsoleMode())
		Console_Print("DialogueCameraMode >> %s", DialogueCameraHandler::GetCameraModeName(DialogueCameraHandler::g_angleMode));
	return true;
}

static ParamInfo kParams_SetDialogueCameraFixedAngle[1] = {
	{"angle", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetDialogueCameraFixedAngle, "Set fixed dialogue camera angle (0-11) used by mode 1", 0, 1, kParams_SetDialogueCameraFixedAngle);

bool Cmd_SetDialogueCameraFixedAngle_Execute(COMMAND_ARGS)
{
	*result = 0;
	int angle = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &angle))
		return true;
	if (!DialogueCameraHandler::SetFixedAngle(angle))
		return true;

	*result = (float)DialogueCameraHandler::g_fixedAngle;

	if (IsConsoleMode())
		Console_Print("DialogueCameraFixedAngle >> %d (%s)", angle, DialogueCameraHandler::GetCameraAngleName(DialogueCameraHandler::g_fixedAngle));
	return true;
}

static ParamInfo kParams_SetDialogueCameraAngle[1] = {
	{"angle", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(SetDialogueCameraAngle, "Switch dialogue camera immediately. Pass -1 for random, 0-11 for exact", 0, 1, kParams_SetDialogueCameraAngle);

bool Cmd_SetDialogueCameraAngle_Execute(COMMAND_ARGS)
{
	*result = 0;
	int angle = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &angle))
		return true;

	int chosenAngle = DialogueCameraHandler::SetCurrentAngle(angle);
	if (chosenAngle < 0)
		return true;

	*result = (float)chosenAngle;

	if (IsConsoleMode())
		Console_Print("DialogueCameraAngle >> %d (%s)", chosenAngle, DialogueCameraHandler::GetCameraAngleName((DialogueCameraHandler::CameraAngle)chosenAngle));
	return true;
}

static ParamInfo kParams_SetDialogueCameraDolly[3] = {
	{"speed", kParamType_Float, 0},
	{"maxDist", kParamType_Float, 0},
	{"runOnce", kParamType_Integer, 1},
};

DEFINE_COMMAND_PLUGIN(SetDialogueCameraDolly, "Set dialogue camera dolly (0 0 to disable). runOnce=1 (default): only first angle", 0, 3, kParams_SetDialogueCameraDolly);

bool Cmd_SetDialogueCameraDolly_Execute(COMMAND_ARGS)
{
	*result = 0;
	float speed = 0.0f, maxDist = 0.0f;
	UInt32 runOnce = 1;
	if (!ExtractArgs(EXTRACT_ARGS, &speed, &maxDist, &runOnce))
		return true;

	DialogueCameraHandler::SetDolly(speed, maxDist, runOnce);
	*result = 1;

	if (IsConsoleMode())
	{
		if (maxDist != 0.0f)
			Console_Print("DialogueCameraDolly >> speed=%.2f maxDist=%.1f runOnce=%d", speed, maxDist, runOnce);
		else
			Console_Print("DialogueCameraDolly >> disabled");
	}
	return true;
}

static ParamInfo kParams_SetDialogueCameraShake[1] = {
	{"amplitude", kParamType_Float, 0},
};

DEFINE_COMMAND_PLUGIN(SetDialogueCameraShake, "Set dialogue camera shake amplitude (-1 for default, 0 to disable)", 0, 1, kParams_SetDialogueCameraShake);

bool Cmd_SetDialogueCameraShake_Execute(COMMAND_ARGS)
{
	*result = 0;
	float amplitude = -1.0f;
	if (!ExtractArgs(EXTRACT_ARGS, &amplitude))
		return true;

	DialogueCameraHandler::SetShakeAmplitude(amplitude);
	*result = 1;

	if (IsConsoleMode())
	{
		if (amplitude < 0.0f)
			Console_Print("DialogueCameraShake >> default (3.0)");
		else if (amplitude == 0.0f)
			Console_Print("DialogueCameraShake >> disabled");
		else
			Console_Print("DialogueCameraShake >> %.2f", amplitude);
	}
	return true;
}

namespace DialogueCameraHandler {
void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraDolly);
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraShake);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraEnabled);
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraMode);
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraFixedAngle);
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraAngle);
}
}
