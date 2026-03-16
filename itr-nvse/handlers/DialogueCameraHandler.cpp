#include "DialogueCameraHandler.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameObjects.h"
#include "internal/SafeWrite.h"
#include "internal/globals.h"
#include "internal/Mat3.h"
#include "PerlinNoise.hpp"
#include <cmath>

//camera hooks - intercept game's own camera update calls
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

	static bool IsRel32Patched(UInt32 src, UInt8 opcode, UInt32 dst) {
		return *(UInt8*)src == opcode && (*(UInt32*)(src + 1) + src + 5) == dst;
	}

	template<typename T>
	static bool PatchRelCall(UInt32 src, UInt32 dst, T& orig, const char* name) {
		if (*(UInt8*)src != 0xE8) {
			Log("DialogueCameraHandler: %s patch site 0x%08X expected CALL, found 0x%02X", name, src, *(UInt8*)src);
			return false;
		}

		orig = (T)(*(UInt32*)(src + 1) + src + 5);
		DWORD oldProtect;
		if (!VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			Log("DialogueCameraHandler: VirtualProtect failed for %s at 0x%08X", name, src);
			return false;
		}

		*(UInt8*)src = 0xE8;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (void*)src, 5);
		return true;
	}

	bool InstallHooks() {
		if (g_hooksInstalled)
			return true;

		//hook addresses (verified in IDA):
		//0x94AD8A - SetLocalTranslate in PlayerCharacter::HandleFlycamMovement
		//0x94AD9D - SetLocalRotate in PlayerCharacter::HandleFlycamMovement
		//0x94BDC2 - SetLocalTranslate in PlayerCharacter::UpdateCamera
		//0x94BDD5 - SetLocalRotate in PlayerCharacter::UpdateCamera
		if (!PatchRelCall(0x94AD8A, (UInt32)TranslateHook1, g_origTranslate1, "HandleFlycamMovement::SetLocalTranslate"))
			return false;
		if (!PatchRelCall(0x94AD9D, (UInt32)RotateHook1, g_origRotate1, "HandleFlycamMovement::SetLocalRotate"))
			return false;
		if (!PatchRelCall(0x94BDC2, (UInt32)TranslateHook2, g_origTranslate2, "UpdateCamera::SetLocalTranslate"))
			return false;
		if (!PatchRelCall(0x94BDD5, (UInt32)RotateHook2, g_origRotate2, "UpdateCamera::SetLocalRotate"))
			return false;

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


static float g_savedFreqs[64] = {0};
static UInt32 g_savedStates[64] = {0};
static int g_savedCount = 0;

static void FreezePlayerAnim(bool freeze)
{
	PlayerCharacter* player = *(PlayerCharacter**)0x11DEA3C;
	if (!player) return;

	void* renderData = *(void**)((UInt8*)player + 0x64);
	if (!renderData) return;
	void* rootNode = *(void**)((UInt8*)renderData + 0x14);
	if (!rootNode) return;

	void* controller = *(void**)((UInt8*)rootNode + 0x18);
	if (!controller) return;

	void** seqData = *(void***)((UInt8*)controller + 0x2C);
	UInt16 seqCount = *(UInt16*)((UInt8*)controller + 0x34);
	if (!seqData || seqCount == 0) return;
	if (seqCount > 64) seqCount = 64;

	if (freeze)
	{
		g_savedCount = seqCount;
		for (int i = 0; i < seqCount; i++)
		{
			if (!seqData[i]) { g_savedFreqs[i] = 1.0f; g_savedStates[i] = 0; continue; }
			g_savedFreqs[i] = *(float*)((UInt8*)seqData[i] + 0x28);
			g_savedStates[i] = *(UInt32*)((UInt8*)seqData[i] + 0x44);
			*(float*)((UInt8*)seqData[i] + 0x28) = 0.0f;
			*(UInt32*)((UInt8*)seqData[i] + 0x44) = 0;
		}
	}
	else
	{
		for (int i = 0; i < g_savedCount && i < (int)seqCount; i++)
		{
			if (!seqData[i]) continue;
			*(float*)((UInt8*)seqData[i] + 0x28) = g_savedFreqs[i];
			*(UInt32*)((UInt8*)seqData[i] + 0x44) = g_savedStates[i];
		}
		g_savedCount = 0;
	}
}

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

//returns safe fraction from look target (1.0=ok, <1.0=pull closer)
static float CheckCameraClip(float camX, float camY, float camZ,
							 float lookX, float lookY, float lookZ,
							 TESObjectREFR* npc, TESObjectREFR* player) {
	float safeFrac = 1.0f;

	if (npc) {
		float npcX = npc->posX;
		float npcY = npc->posY;
		float npcZ = npc->posZ + 60.0f;
		float dx = camX - npcX;
		float dy = camY - npcY;
		float dz = camZ - npcZ;
		float distToNPC = sqrtf(dx*dx + dy*dy + dz*dz);

		constexpr float kMinNPCDist = 40.0f;
		if (distToNPC < kMinNPCDist) {
			float lookToCamX = camX - lookX;
			float lookToCamY = camY - lookY;
			float lookToCamZ = camZ - lookZ;
			float totalDist = sqrtf(lookToCamX*lookToCamX + lookToCamY*lookToCamY + lookToCamZ*lookToCamZ);
			if (totalDist > 1.0f) {
				float adjustedFrac = (totalDist - (kMinNPCDist - distToNPC) * 1.5f) / totalDist;
				if (adjustedFrac < safeFrac) {
					safeFrac = adjustedFrac;
				}
			}
		}
	}

	if (player) {
		float plrX = player->posX;
		float plrY = player->posY;
		float plrZ = player->posZ + 60.0f;
		float dx = camX - plrX;
		float dy = camY - plrY;
		float dz = camZ - plrZ;
		float distToPlayer = sqrtf(dx*dx + dy*dy + dz*dz);

		constexpr float kMinPlayerDist = 35.0f;
		if (distToPlayer < kMinPlayerDist) {
			float lookToCamX = camX - lookX;
			float lookToCamY = camY - lookY;
			float lookToCamZ = camZ - lookZ;
			float totalDist = sqrtf(lookToCamX*lookToCamX + lookToCamY*lookToCamY + lookToCamZ*lookToCamZ);
			if (totalDist > 1.0f) {
				float adjustedFrac = (totalDist - (kMinPlayerDist - distToPlayer) * 1.5f) / totalDist;
				if (adjustedFrac < safeFrac) {
					safeFrac = adjustedFrac;
				}
			}
		}
	}

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

static NVSEConsoleInterface* g_console = nullptr;
static PlayerCharacter** g_thePlayer = (PlayerCharacter**)0x11DEA3C;

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
static CameraAngle g_currentAngle = kAngle_OverShoulder;
static bool g_cameraActive = false;
static bool g_wasFirstPerson = false;
static int g_dialogueLineCount = 0;
static UInt32 g_lastTopicInfoID = 0;

static float g_baseCamX = 0, g_baseCamY = 0, g_baseCamZ = 0;
static float g_baseLookX = 0, g_baseLookY = 0, g_baseLookZ = 0;
static double g_noiseTime = 0;
static bool g_patchesInstalled = false;

static const siv::PerlinNoise g_perlinPitch{ 4 };
static const siv::PerlinNoise g_perlinYaw{ 5 };

static bool IsNopRange(UInt32 addr, UInt32 size) {
	for (UInt32 i = 0; i < size; i++) {
		if (*(UInt8*)(addr + i) != 0x90)
			return false;
	}
	return true;
}

static bool ValidateNopSite(UInt32 addr, UInt32 size, UInt8 expectedOpcode, const char* name) {
	if (IsNopRange(addr, size))
		return true;
	if (*(UInt8*)addr != expectedOpcode) {
		Log("DialogueCameraHandler: %s patch site 0x%08X expected opcode 0x%02X or NOPs, found 0x%02X", name, addr, expectedOpcode, *(UInt8*)addr);
		return false;
	}
	return true;
}

static bool ValidateRelJumpSite(UInt32 addr, UInt32 dst, const char* name) {
	UInt8 opcode = *(UInt8*)addr;
	if (CameraHooks::IsRel32Patched(addr, 0xE9, dst))
		return true;
	if (opcode == 0xE9) {
		Log("DialogueCameraHandler: %s patch site 0x%08X already jumps to 0x%08X", name, addr, *(UInt32*)(addr + 1) + addr + 5);
		return false;
	}
	return true;
}

static bool ValidateBytePatchSite(UInt32 addr, UInt8 patchedValue, UInt8 allowedOriginalA, UInt8 allowedOriginalB, const char* name) {
	UInt8 current = *(UInt8*)addr;
	if (current == patchedValue)
		return true;
	if (current != allowedOriginalA && current != allowedOriginalB) {
		Log("DialogueCameraHandler: %s patch site 0x%08X expected 0x%02X/0x%02X or patched 0x%02X, found 0x%02X", name, addr, allowedOriginalA, allowedOriginalB, patchedValue, current);
		return false;
	}
	return true;
}

static bool InstallDialoguePatches() {
	if (g_patchesInstalled)
		return true;

	if (!ValidateNopSite(0x953124, 5, 0xE8, "ForceThirdPerson start"))
		return false;
	if (!ValidateNopSite(0x761DEF, 5, 0xE8, "ForceThirdPerson end"))
		return false;
	if (!ValidateRelJumpSite(0x953ABF, 0x953AF5, "ForceThirdPerson branch 1"))
		return false;
	if (!ValidateRelJumpSite(0x762E55, 0x762E72, "ForceThirdPerson branch 2"))
		return false;
	if (!ValidateRelJumpSite(0x9533BE, 0x953562, "DisableDialogueZoom branch"))
		return false;
	if (!ValidateBytePatchSite(0x953BBA, 0xEB, 0x74, 0x75, "DisableDialogueZoom conditional"))
		return false;

	SafeWrite::WriteNop(0x953124, 5);
	SafeWrite::WriteNop(0x761DEF, 5);
	SafeWrite::WriteRelJump(0x953ABF, 0x953AF5);
	SafeWrite::WriteRelJump(0x762E55, 0x762E72);
	SafeWrite::WriteRelJump(0x9533BE, 0x953562);
	SafeWrite::Write8(0x953BBA, 0xEB);

	g_patchesInstalled = true;
	return true;
}

static void DisableCamera() {
	CameraHooks::Disable();
}

typedef void* (__cdecl* GetObjectByName_t)(void* rootNode, const char* name);
static GetObjectByName_t GetObjectByName = (GetObjectByName_t)0x4AAE30;

static bool GetHeadPos(TESObjectREFR* ref, float& outX, float& outY, float& outZ)
{
	if (!ref) return false;
	void* renderData = *(void**)((UInt8*)ref + 0x64);
	if (!renderData) return false;
	void* rootNode = *(void**)((UInt8*)renderData + 0x14);
	if (!rootNode) return false;

	//try head bone, fall back to neck
	void* bone = GetObjectByName(rootNode, "Bip01 Head");
	if (!bone) bone = GetObjectByName(rootNode, "Bip01 Neck1");
	if (!bone) return false;

	float* worldPos = (float*)((UInt8*)bone + 0x8C);
	outX = worldPos[0];
	outY = worldPos[1];
	outZ = worldPos[2];
	return true;
}

static void ApplyCameraAngle(CameraAngle angle) {
	PlayerCharacter* player = *g_thePlayer;
	if (!player || !g_dialogueTarget) return;

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

float g_dollySpeed = 0.0f;
float g_dollyMaxDist = 0.0f;
int g_dollyRunOnce = 1;
static float g_dollyProgress = 0.0f;
static bool g_dollyFirstDone = false;
float g_shakeAmplitude = -1.0f; //-1 = use default

static void ApplyCameraNoise() {
	float rotAmp = (g_shakeAmplitude >= 0.0f) ? g_shakeAmplitude : 3.0f;

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
			g_dollyProgress += g_dollySpeed * 0.001f;
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

	PlayerCharacter* player = *g_thePlayer;
	if (!player) return;

	g_wasFirstPerson = !*(bool*)((UInt8*)player + 0x64A); //is3rdPerson
	if (g_wasFirstPerson) {
		SetFirstPerson(player, false);
	}

	FreezePlayerAnim(true);

	g_currentAngle = kAngle_Vanilla;
	g_dialogueLineCount = 0;
	g_lastTopicInfoID = 0;
	g_dollyProgress = 0.0f;
	g_dollyFirstDone = false;
	g_cameraActive = true;

	ApplyCameraAngle(g_currentAngle);
}

static void OnDialogueEnd() {
	if (g_cameraActive) {
		FreezePlayerAnim(false);
		DisableCamera();
		if (g_wasFirstPerson) {
			PlayerCharacter* player = *g_thePlayer;
			if (player) {
				SetFirstPerson(player, true);
			}
		}
		g_cameraActive = false;
	}
	g_dialogueTarget = nullptr;
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

	FreezePlayerAnim(true);

	DialogueMenu* dlgMenu = DialogueMenu::GetSingleton();
	if (dlgMenu) {
		UInt32 currentInfoAddr = (UInt32)dlgMenu->currentInfo;
		if (currentInfoAddr != 0 && currentInfoAddr != g_lastTopicInfoID) {
			g_lastTopicInfoID = currentInfoAddr;
			g_dialogueLineCount++;
			g_currentAngle = (CameraAngle)(g_dialogueLineCount % kAngle_Count);
			if (!g_dollyRunOnce || !g_dollyFirstDone)
				g_dollyProgress = 0.0f;
			ApplyCameraAngle(g_currentAngle);
		}
	}

	g_noiseTime += 0.005;
	ApplyCameraNoise();
}

bool Init(NVSEConsoleInterface* console) {
	g_console = console;
	return true;
}

bool InstallCameraHooks() {
	if (!InstallDialoguePatches())
		return false;
	return CameraHooks::InstallHooks();
}

}

bool DCH_Init(void* nvse) {
	NVSEInterface* nvseIntfc = (NVSEInterface*)nvse;
	NVSEConsoleInterface* console = (NVSEConsoleInterface*)nvseIntfc->QueryInterface(kInterface_Console);
	return DialogueCameraHandler::Init(console);
}

void DCH_Update() {
	DialogueCameraHandler::Update();
}

bool DCH_InstallCameraHooks() {
	return DialogueCameraHandler::InstallCameraHooks();
}

void DCH_SetExternalRotation(const Mat3& rot) {
	CameraHooks::SetExternalRotation(rot);
}

void DCH_ClearExternalRotation() {
	CameraHooks::ClearExternalRotation();
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

	DialogueCameraHandler::g_dollySpeed = speed;
	DialogueCameraHandler::g_dollyMaxDist = maxDist;
	DialogueCameraHandler::g_dollyRunOnce = runOnce;
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

	DialogueCameraHandler::g_shakeAmplitude = amplitude;
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

void DCH_RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraDolly);
	nvse->RegisterCommand(&kCommandInfo_SetDialogueCameraShake);
}
