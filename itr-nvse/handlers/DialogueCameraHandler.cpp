#include "DialogueCameraHandler.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameObjects.h"
#include "internal/SafeWrite.h"
#include "PerlinNoise.hpp"
#include <cmath>

//camera hooks - intercept game's own camera update calls
namespace CameraHooks {
	struct NiVector3 { float x, y, z; };
	struct NiMatrix3 {
		float m[3][3];

		void MakeIdentity() {
			m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
			m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
			m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
		}

		void MakeXRotation(float rad) {
			float c = cosf(rad), s = sinf(rad);
			m[0][0] = 1; m[0][1] = 0;  m[0][2] = 0;
			m[1][0] = 0; m[1][1] = c;  m[1][2] = s;
			m[2][0] = 0; m[2][1] = -s; m[2][2] = c;
		}

		void MakeZRotation(float rad) {
			float c = cosf(rad), s = sinf(rad);
			m[0][0] = c;  m[0][1] = s; m[0][2] = 0;
			m[1][0] = -s; m[1][1] = c; m[1][2] = 0;
			m[2][0] = 0;  m[2][1] = 0; m[2][2] = 1;
		}

		NiMatrix3 operator*(const NiMatrix3& b) const {
			NiMatrix3 r;
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					r.m[i][j] = m[i][0]*b.m[0][j] + m[i][1]*b.m[1][j] + m[i][2]*b.m[2][j];
				}
			}
			return r;
		}
	};

	static NiVector3 g_cameraPos = {0, 0, 0};
	static NiMatrix3 g_cameraRot;
	static bool g_overrideActive = false;

	typedef void (__fastcall *SetLocalTranslate_t)(void*, void*, NiVector3*);
	typedef void (__fastcall *SetLocalRotate_t)(void*, void*, NiMatrix3*);

	static SetLocalTranslate_t g_origTranslate1 = nullptr;
	static SetLocalTranslate_t g_origTranslate2 = nullptr;
	static SetLocalRotate_t g_origRotate1 = nullptr;
	static SetLocalRotate_t g_origRotate2 = nullptr;

	void __fastcall TranslateHook1(void* node, void* edx, NiVector3* pos) {
		if (g_overrideActive) pos = &g_cameraPos;
		g_origTranslate1(node, edx, pos);
	}

	void __fastcall TranslateHook2(void* node, void* edx, NiVector3* pos) {
		if (g_overrideActive) pos = &g_cameraPos;
		g_origTranslate2(node, edx, pos);
	}

	void __fastcall RotateHook1(void* node, void* edx, NiMatrix3* rot) {
		if (g_overrideActive) rot = &g_cameraRot;
		g_origRotate1(node, edx, rot);
	}

	void __fastcall RotateHook2(void* node, void* edx, NiMatrix3* rot) {
		if (g_overrideActive) rot = &g_cameraRot;
		g_origRotate2(node, edx, rot);
	}

	void SetPosition(float x, float y, float z, float pitchDeg, float yawDeg) {
		g_cameraPos.x = x;
		g_cameraPos.y = y;
		g_cameraPos.z = z;

		//pitch = rotation around X axis, yaw = rotation around Z axis
		float pitchRad = pitchDeg * (3.14159265f / 180.0f);
		float yawRad = yawDeg * (3.14159265f / 180.0f);

		NiMatrix3 rotX, rotZ;
		rotX.MakeXRotation(pitchRad);
		rotZ.MakeZRotation(yawRad);
		g_cameraRot = rotZ * rotX; //yaw then pitch

		g_overrideActive = true;
	}

	void Disable() {
		g_overrideActive = false;
	}

	//helper to write call and get original
	template<typename T>
	static T WriteRelCallEx(UInt32 src, UInt32 dst) {
		T orig = (T)(*(UInt32*)(src + 1) + src + 5);
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE8;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
		return orig;
	}

	void InstallHooks() {
		//hook addresses (verified in IDA):
		//0x94AD8A - SetLocalTranslate in PlayerCharacter::HandleFlycamMovement
		//0x94AD9D - SetLocalRotate in PlayerCharacter::HandleFlycamMovement
		//0x94BDC2 - SetLocalTranslate in PlayerCharacter::UpdateCamera
		//0x94BDD5 - SetLocalRotate in PlayerCharacter::UpdateCamera
		g_origTranslate1 = WriteRelCallEx<SetLocalTranslate_t>(0x94AD8A, (UInt32)TranslateHook1);
		g_origRotate1 = WriteRelCallEx<SetLocalRotate_t>(0x94AD9D, (UInt32)RotateHook1);
		g_origTranslate2 = WriteRelCallEx<SetLocalTranslate_t>(0x94BDC2, (UInt32)TranslateHook2);
		g_origRotate2 = WriteRelCallEx<SetLocalRotate_t>(0x94BDD5, (UInt32)RotateHook2);

		g_cameraRot.MakeIdentity();
	}
}

namespace DialogueCameraHandler {

//0x950110 = PlayerCharacter::SetFirstPerson(bool abFirst)
typedef void (__thiscall *SetFirstPerson_t)(void* player, bool bFirst);
static SetFirstPerson_t SetFirstPerson = (SetFirstPerson_t)0x950110;

typedef void* (__thiscall *GetAnimData_t)(Actor* actor); //vtable 0x1E4

typedef UInt16 (__thiscall *GetAnimGroupForAction_t)(Actor* actor, UInt32 action, UInt32 unk1, UInt32 unk2, void* animData);
static GetAnimGroupForAction_t GetAnimGroupForAction = (GetAnimGroupForAction_t)0x897910;

typedef void (__thiscall *PlayAnimSeq_t)(void* animData, UInt32 seqID, UInt32 flags, SInt32 unk1, SInt32 unk2);
static PlayAnimSeq_t PlayAnimSeq = (PlayAnimSeq_t)0x494740;

typedef void (__thiscall *AnimDataRefresh_t)(void* animData, UInt32 unk);
static AnimDataRefresh_t AnimDataRefresh = (AnimDataRefresh_t)0x499240;

static void ForcePlayerIdle(Actor* player) {
	void** vtable = *(void***)player;
	GetAnimData_t getAnimData = (GetAnimData_t)vtable[0x1E4/4];
	UInt8* animData = (UInt8*)getAnimData(player);
	if (!animData) return;

	void** animSequences = (void**)(animData + 0xE0); //animSequences array
	animSequences[1] = nullptr; //kSequence_Movement


	UInt16 seqID = GetAnimGroupForAction(player, 0, 0, 0, animData);
	if (seqID != 0xFFFF) {
		PlayAnimSeq(animData, seqID, 1, -1, -1);
	}
}

constexpr float PI = 3.14159265358979323846f;
constexpr float kHavokScale = 0.1428571f; //1/7, game units to havok

constexpr UInt8 LAYER_STATIC = 1;
constexpr UInt8 LAYER_BIPED = 8;
constexpr UInt8 LAYER_LINEOFSIGHT = 37;

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
static float* g_gameSecondsPassed = (float*)0x11DEA14;
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

static const siv::PerlinNoise g_perlinX{ 1 };
static const siv::PerlinNoise g_perlinY{ 2 };
static const siv::PerlinNoise g_perlinZ{ 3 };
static const siv::PerlinNoise g_perlinPitch{ 4 };
static const siv::PerlinNoise g_perlinYaw{ 5 };

static void SetCameraPosition(float x, float y, float z, float pitch, float yaw) {
	CameraHooks::SetPosition(x, y, z, pitch, yaw);
}

static void DisableCamera() {
	CameraHooks::Disable();
}

static void ApplyCameraAngle(CameraAngle angle) {
	PlayerCharacter* player = *g_thePlayer;
	if (!player || !g_dialogueTarget) return;

	float px = player->posX;
	float py = player->posY;
	float pz = player->posZ + 60.0f;

	float nx = g_dialogueTarget->posX;
	float ny = g_dialogueTarget->posY;
	float nz = g_dialogueTarget->posZ + 60.0f;

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

	struct CamCandidate {
		float x, y, z;             //camera position
		float lookX, lookY, lookZ; //where to look
	};
	CamCandidate candidates[2];

	switch (angle) {
		case kAngle_Vanilla:
			candidates[0] = { px + dirX * 5.0f, py + dirY * 5.0f, pz + 50.0f, nx, ny, nz + 10.0f };
			candidates[1] = candidates[0];
			break;
		case kAngle_OverShoulder:
			candidates[0] = { px - dirX * 70.0f + perpX * 50.0f, py - dirY * 70.0f + perpY * 50.0f, pz + 20.0f, nx, ny, nz };
			candidates[1] = { px - dirX * 70.0f - perpX * 50.0f, py - dirY * 70.0f - perpY * 50.0f, pz + 20.0f, nx, ny, nz };
			break;
		case kAngle_NPCCloseup:
			candidates[0] = { nx + dirX * 40.0f + perpX * 80.0f, ny + dirY * 40.0f + perpY * 80.0f, nz + 40.0f, px, py, pz };
			candidates[1] = { nx + dirX * 40.0f - perpX * 80.0f, ny + dirY * 40.0f - perpY * 80.0f, nz + 40.0f, px, py, pz };
			break;
		case kAngle_TwoShot:
			candidates[0] = { midX + perpX * 120.0f, midY + perpY * 120.0f, midZ + 20.0f, midX, midY, midZ - 10.0f };
			candidates[1] = { midX - perpX * 120.0f, midY - perpY * 120.0f, midZ + 20.0f, midX, midY, midZ - 10.0f };
			break;
		case kAngle_NPCFace:
			candidates[0] = { nx - dirX * 60.0f + perpX * 20.0f, ny - dirY * 60.0f + perpY * 20.0f, nz + 60.0f, nx, ny, nz };
			candidates[1] = { nx - dirX * 60.0f - perpX * 20.0f, ny - dirY * 60.0f - perpY * 20.0f, nz + 60.0f, nx, ny, nz };
			break;
		case kAngle_LowAngle:
			candidates[0] = { nx - dirX * 80.0f + perpX * 30.0f, ny - dirY * 80.0f + perpY * 30.0f, nz - 30.0f, nx, ny, nz + 30.0f };
			candidates[1] = { nx - dirX * 80.0f - perpX * 30.0f, ny - dirY * 80.0f - perpY * 30.0f, nz - 30.0f, nx, ny, nz + 30.0f };
			break;
		case kAngle_HighAngle:
			candidates[0] = { nx - dirX * 60.0f + perpX * 30.0f, ny - dirY * 60.0f + perpY * 30.0f, nz + 80.0f, nx, ny, nz };
			candidates[1] = { nx - dirX * 60.0f - perpX * 30.0f, ny - dirY * 60.0f - perpY * 30.0f, nz + 80.0f, nx, ny, nz };
			break;
		case kAngle_PlayerFace:
			candidates[0] = { px + perpX * 80.0f, py + perpY * 80.0f, pz + 30.0f, nx, ny, nz };
			candidates[1] = { px - perpX * 80.0f, py - perpY * 80.0f, pz + 30.0f, nx, ny, nz };
			break;
		case kAngle_WideShot:
			candidates[0] = { midX + perpX * 200.0f, midY + perpY * 200.0f, midZ + 60.0f, midX, midY, midZ };
			candidates[1] = { midX - perpX * 200.0f, midY - perpY * 200.0f, midZ + 60.0f, midX, midY, midZ };
			break;
		case kAngle_NPCProfile:
			candidates[0] = { nx + perpX * 90.0f, ny + perpY * 90.0f, nz + 40.0f, nx, ny, nz + 15.0f };
			candidates[1] = { nx - perpX * 90.0f, ny - perpY * 90.0f, nz + 40.0f, nx, ny, nz + 15.0f };
			break;
		case kAngle_PlayerProfile:
			candidates[0] = { px + perpX * 90.0f, py + perpY * 90.0f, pz + 40.0f, px, py, pz + 15.0f };
			candidates[1] = { px - perpX * 90.0f, py - perpY * 90.0f, pz + 40.0f, px, py, pz + 15.0f };
			break;
		case kAngle_Overhead:
			candidates[0] = { midX, midY, midZ + 150.0f, midX, midY, midZ - 20.0f };
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

	static const char* angleNames[] = {
		"Vanilla", "OverShoulder", "NPCCloseup", "TwoShot", "NPCFace", "LowAngle",
		"HighAngle", "PlayerFace", "WideShot", "NPCProfile", "PlayerProfile", "Overhead"
	};

	CameraHooks::SetPosition(camX, camY, camZ, pitch, yaw);
}

static void ApplyCameraNoise() {
	constexpr float kRotAmp = 3.0f;

	float toDirX = g_baseLookX - g_baseCamX;
	float toDirY = g_baseLookY - g_baseCamY;
	float toDirZ = g_baseLookZ - g_baseCamZ;
	float horizDist = sqrtf(toDirX*toDirX + toDirY*toDirY);

	float yaw = atan2f(toDirX, toDirY) * (180.0f / PI);
	float pitch = -atan2f(toDirZ, horizDist) * (180.0f / PI);

	yaw += (float)g_perlinYaw.octave1D(g_noiseTime, 3, 0.5) * kRotAmp;
	pitch += (float)g_perlinPitch.octave1D(g_noiseTime, 3, 0.5) * kRotAmp * 0.5f;

	CameraHooks::SetPosition(g_baseCamX, g_baseCamY, g_baseCamZ, pitch, yaw);
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

	g_wasFirstPerson = !*(bool*)((UInt8*)player + 0x650); //bThirdPerson
	if (g_wasFirstPerson) {
		SetFirstPerson(player, false);
	}

	ForcePlayerIdle(player);

	g_currentAngle = kAngle_Vanilla;
	g_dialogueLineCount = 0;
	g_lastTopicInfoID = 0;
	g_cameraActive = true;

	ApplyCameraAngle(g_currentAngle);
}

static void OnDialogueEnd() {
	if (g_cameraActive) {
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

	PlayerCharacter* player = *g_thePlayer;
	if (player) ForcePlayerIdle(player);

	DialogueMenu* dlgMenu = DialogueMenu::GetSingleton();
	if (dlgMenu) {
		UInt32 currentInfoAddr = (UInt32)dlgMenu->currentInfo;
		if (currentInfoAddr != 0 && currentInfoAddr != g_lastTopicInfoID) {
			g_lastTopicInfoID = currentInfoAddr;
			g_dialogueLineCount++;
			g_currentAngle = (CameraAngle)(g_dialogueLineCount % kAngle_Count);
			ApplyCameraAngle(g_currentAngle);
		}
	}

	g_noiseTime += 0.005;
	ApplyCameraNoise();
}

bool Init(NVSEConsoleInterface* console) {
	g_console = console;

	//force 3rd person in dialogue (ported from RealTimeMenus)
	SafeWrite::WriteNop(0x953124, 5);
	SafeWrite::WriteNop(0x761DEF, 5);
	SafeWrite::WriteRelJump(0x953ABF, 0x953AF5);
	SafeWrite::WriteRelJump(0x762E55, 0x762E72);

	SafeWrite::WriteRelJump(0x9533BE, 0x953562); //disable dialogue zoom
	SafeWrite::Write8(0x953BBA, 0xEB);

	return true;
}

void InstallCameraHooks() {
	CameraHooks::InstallHooks();
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

void DCH_InstallCameraHooks() {
	DialogueCameraHandler::InstallCameraHooks();
}
