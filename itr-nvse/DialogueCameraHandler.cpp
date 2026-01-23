#include "DialogueCameraHandler.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameObjects.h"
#include "PerlinNoise.hpp"
#include <cmath>
#include <cstdio>
#include <Windows.h>

//CameraOverride API from main.cpp
namespace CameraOverride {
	void SetRotation(bool enable, int axis, float degrees);
	void SetTranslation(bool enable, float x, float y, float z);
	void ResetRotation();
}

static FILE* g_log = nullptr;
static void Log(const char* fmt, ...) {
	if (!g_log) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);
	fprintf(g_log, "\n");
	fflush(g_log);
}

namespace DialogueCameraHandler {

//PlayerCharacter::SetFirstPerson(bool abFirst) - true=1st, false=3rd
typedef void (__thiscall *SetFirstPerson_t)(void* player, bool bFirst);
static SetFirstPerson_t SetFirstPerson = (SetFirstPerson_t)0x950110;

//Actor::GetAnimData - virtual at 0x1E4
typedef void* (__thiscall *GetAnimData_t)(Actor* actor);

//internal anim functions
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

	//clear movement sequence (index 1) at offset 0xE0
	void** animSequences = (void**)(animData + 0xE0);
	animSequences[1] = nullptr; //kSequence_Movement

	//play idle animation (group 0)
	UInt16 seqID = GetAnimGroupForAction(player, 0, 0, 0, animData);
	if (seqID != 0xFFFF) {
		PlayAnimSeq(animData, seqID, 1, -1, -1);
	}
}

constexpr float PI = 3.14159265358979323846f;
constexpr float kHavokScale = 0.1428571f; //game units to havok units (1/7)

//collision layer types
constexpr UInt8 LAYER_STATIC = 1;
constexpr UInt8 LAYER_BIPED = 8;
constexpr UInt8 LAYER_LINEOFSIGHT = 37;

//RayCastData structure for TES::PickObject
struct alignas(16) RayCastData {
	float pos0[4];       //0x00 - start position (havok units)
	float pos1[4];       //0x10 - end position (havok units)
	UInt8 byte20;        //0x20
	UInt8 pad21[3];
	UInt8 layerType;     //0x24
	UInt8 filterFlags;   //0x25
	UInt16 group;        //0x26
	UInt32 unk28[6];     //0x28
	float hitFraction;   //0x40 - result: 0.0-1.0 along ray (1.0 = no hit)
	UInt32 unk44[15];    //0x44
	void* cdBody;        //0x80 - result: collision body that was hit
	UInt32 unk84[3];     //0x84
	float vector90[4];   //0x90
	UInt32 unkA0[3];     //0xA0
	UInt8 byteAC;        //0xAC
	UInt8 padAD[3];      //0xAD
};
static_assert(sizeof(RayCastData) == 0xB0, "RayCastData size mismatch");

//TES::PickObject at 0x458440
typedef void (__thiscall *TES_PickObject_t)(void* tes, RayCastData* rcData, bool unk);
static TES_PickObject_t TES_PickObject = (TES_PickObject_t)0x458440;
static void** g_TES = (void**)0x11DEA10;

//cast ray and return hit fraction (1.0 = no hit, <1.0 = hit at that fraction of distance)
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

//check if camera position is valid (not clipping into geometry or too close to NPC)
//returns safe distance multiplier from look target (1.0 = ok, <1.0 = must pull camera closer to look target)
static float CheckCameraClip(float camX, float camY, float camZ,
							 float lookX, float lookY, float lookZ,
							 TESObjectREFR* npc, TESObjectREFR* player) {
	float safeFrac = 1.0f;

	//check distance to NPC - don't let camera get too close
	if (npc) {
		float npcX = npc->posX;
		float npcY = npc->posY;
		float npcZ = npc->posZ + 60.0f; //head height
		float dx = camX - npcX;
		float dy = camY - npcY;
		float dz = camZ - npcZ;
		float distToNPC = sqrtf(dx*dx + dy*dy + dz*dz);

		constexpr float kMinNPCDist = 40.0f; //minimum distance from NPC center
		if (distToNPC < kMinNPCDist) {
			//calculate how far along the look->cam vector we need to be
			float lookToCamX = camX - lookX;
			float lookToCamY = camY - lookY;
			float lookToCamZ = camZ - lookZ;
			float totalDist = sqrtf(lookToCamX*lookToCamX + lookToCamY*lookToCamY + lookToCamZ*lookToCamZ);
			if (totalDist > 1.0f) {
				float adjustedFrac = (totalDist - (kMinNPCDist - distToNPC) * 1.5f) / totalDist;
				if (adjustedFrac < safeFrac) {
					safeFrac = adjustedFrac;
					Log("CameraClip: too close to NPC (dist=%.1f), adjusting frac to %.3f", distToNPC, safeFrac);
				}
			}
		}
	}

	//check distance to player - don't let camera get too close
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
					Log("CameraClip: too close to player (dist=%.1f), adjusting frac to %.3f", distToPlayer, safeFrac);
				}
			}
		}
	}

	//raycast from look target to camera for walls/static geometry
	//offset ray start to be outside actor collision (50 units toward camera)
	float rayDirX = camX - lookX;
	float rayDirY = camY - lookY;
	float rayDirZ = camZ - lookZ;
	float rayLen = sqrtf(rayDirX*rayDirX + rayDirY*rayDirY + rayDirZ*rayDirZ);
	if (rayLen > 60.0f) {
		//normalize and offset start by 50 units
		float offsetDist = 50.0f;
		float startX = lookX + (rayDirX / rayLen) * offsetDist;
		float startY = lookY + (rayDirY / rayLen) * offsetDist;
		float startZ = lookZ + (rayDirZ / rayLen) * offsetDist;

		float staticHit = CastRay(startX, startY, startZ, camX, camY, camZ, LAYER_STATIC);
		if (staticHit < 0.95f) {
			//convert hit fraction back to full ray length
			float remainingLen = rayLen - offsetDist;
			float hitDist = offsetDist + staticHit * remainingLen;
			float fullFrac = (hitDist / rayLen) * 0.85f; //with margin
			if (fullFrac < safeFrac) {
				safeFrac = fullFrac;
				Log("CameraClip: wall hit at frac %.3f, adjusting to %.3f", staticHit, safeFrac);
			}
		}
	}

	//ensure minimum fraction
	if (safeFrac < 0.15f) safeFrac = 0.15f;

	return safeFrac;
}

static void SafeWrite8(UInt32 addr, UInt8 val) {
	DWORD oldProtect;
	VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
	*(UInt8*)addr = val;
	VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
}

static void PatchMemoryNop(UInt32 addr, UInt32 size) {
	DWORD oldProtect;
	VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
	memset((void*)addr, 0x90, size);
	VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
}

static void WriteRelJump(UInt32 src, UInt32 dst) {
	DWORD oldProtect;
	VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
	*(UInt8*)src = 0xE9;
	*(UInt32*)(src + 1) = dst - src - 5;
	VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
}

static void WriteRelCall(UInt32 src, UInt32 dst) {
	DWORD oldProtect;
	VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
	*(UInt8*)src = 0xE8;
	*(UInt32*)(src + 1) = dst - src - 5;
	VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
}

enum CameraAngle {
	kAngle_Vanilla,        //player POV, looking at NPC
	kAngle_OverShoulder,   //behind player's shoulder
	kAngle_NPCCloseup,     //behind NPC looking at player
	kAngle_TwoShot,        //side view of both
	kAngle_NPCFace,        //close-up on NPC face
	kAngle_LowAngle,       //low looking up at NPC
	kAngle_HighAngle,      //high looking down at NPC
	kAngle_PlayerFace,     //close-up on player face
	kAngle_WideShot,       //far back showing both
	kAngle_NPCProfile,     //side profile of NPC
	kAngle_PlayerProfile,  //side profile of player
	kAngle_Overhead,       //above looking down at both
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

//DialogueMenu structure - we need to track when dialogue advances
struct DialogueMenu {
	UInt8 pad[0x48];
	void* currentInfo;  //0x48 - changes per dialogue line

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

//camera noise state
static float g_baseCamX = 0, g_baseCamY = 0, g_baseCamZ = 0;
static float g_baseLookX = 0, g_baseLookY = 0, g_baseLookZ = 0;
static double g_noiseTime = 0;

//perlin noise generators (different seeds for each axis)
static const siv::PerlinNoise g_perlinX{ 1 };
static const siv::PerlinNoise g_perlinY{ 2 };
static const siv::PerlinNoise g_perlinZ{ 3 };
static const siv::PerlinNoise g_perlinPitch{ 4 };
static const siv::PerlinNoise g_perlinYaw{ 5 };

static void RunCommand(const char* fmt, ...) {
	if (!g_console) return;
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	g_console->RunScriptLine(buf, nullptr);
}

static void SetCameraPosition(float x, float y, float z, float pitch, float yaw) {
	Log("SetCameraPosition: x=%.2f y=%.2f z=%.2f pitch=%.2f yaw=%.2f", x, y, z, pitch, yaw);
	char buf[256];
	snprintf(buf, sizeof(buf), "SetCameraTranslate 1 %.2f %.2f %.2f", x, y, z);
	Log("Running: %s", buf);
	if (g_console) g_console->RunScriptLine(buf, nullptr);

	if (g_console) g_console->RunScriptLine("SetCameraRotate 1 0 0", nullptr);
	snprintf(buf, sizeof(buf), "SetCameraRotate 1 1 %.2f", pitch);
	Log("Running: %s", buf);
	if (g_console) g_console->RunScriptLine(buf, nullptr);
	snprintf(buf, sizeof(buf), "SetCameraRotate 1 3 %.2f", yaw);
	Log("Running: %s", buf);
	if (g_console) g_console->RunScriptLine(buf, nullptr);
}

static void DisableCamera() {
	RunCommand("SetCameraTranslate 0 0 0 0");
	RunCommand("SetCameraRotate 0 0 0");
}

static void ApplyCameraAngle(CameraAngle angle) {
	PlayerCharacter* player = *g_thePlayer;
	if (!player || !g_console || !g_dialogueTarget) return;

	//get positions (head height ~60 units above feet)
	float px = player->posX;
	float py = player->posY;
	float pz = player->posZ + 60.0f;

	float nx = g_dialogueTarget->posX;
	float ny = g_dialogueTarget->posY;
	float nz = g_dialogueTarget->posZ + 60.0f;

	//direction from player to npc
	float dx = nx - px;
	float dy = ny - py;
	float dist = sqrtf(dx*dx + dy*dy);
	if (dist < 1.0f) dist = 1.0f;

	//normalize
	float dirX = dx / dist;
	float dirY = dy / dist;

	//perpendicular (for side positioning)
	float perpX = -dirY;
	float perpY = dirX;

	//midpoint between player and npc
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
			//vanilla-style: above player head, looking at NPC face
			candidates[0] = { px + dirX * 5.0f, py + dirY * 5.0f, pz + 50.0f, nx, ny, nz + 10.0f };
			candidates[1] = candidates[0];
			break;
		case kAngle_OverShoulder:
			//behind player's shoulder, looking at NPC
			candidates[0] = { px - dirX * 70.0f + perpX * 50.0f, py - dirY * 70.0f + perpY * 50.0f, pz + 20.0f, nx, ny, nz };
			candidates[1] = { px - dirX * 70.0f - perpX * 50.0f, py - dirY * 70.0f - perpY * 50.0f, pz + 20.0f, nx, ny, nz };
			break;
		case kAngle_NPCCloseup:
			//3/4 view of NPC from side-behind, looking at player
			candidates[0] = { nx + dirX * 40.0f + perpX * 80.0f, ny + dirY * 40.0f + perpY * 80.0f, nz + 40.0f, px, py, pz };
			candidates[1] = { nx + dirX * 40.0f - perpX * 80.0f, ny + dirY * 40.0f - perpY * 80.0f, nz + 40.0f, px, py, pz };
			break;
		case kAngle_TwoShot:
			//side view, looking at midpoint
			candidates[0] = { midX + perpX * 120.0f, midY + perpY * 120.0f, midZ + 20.0f, midX, midY, midZ - 10.0f };
			candidates[1] = { midX - perpX * 120.0f, midY - perpY * 120.0f, midZ + 20.0f, midX, midY, midZ - 10.0f };
			break;
		case kAngle_NPCFace:
			//close-up on NPC face, camera in front of NPC (towards player)
			candidates[0] = { nx - dirX * 60.0f + perpX * 20.0f, ny - dirY * 60.0f + perpY * 20.0f, nz + 60.0f, nx, ny, nz };
			candidates[1] = { nx - dirX * 60.0f - perpX * 20.0f, ny - dirY * 60.0f - perpY * 20.0f, nz + 60.0f, nx, ny, nz };
			break;
		case kAngle_LowAngle:
			//low angle looking up at NPC (dramatic), camera in front and low
			candidates[0] = { nx - dirX * 80.0f + perpX * 30.0f, ny - dirY * 80.0f + perpY * 30.0f, nz - 30.0f, nx, ny, nz + 30.0f };
			candidates[1] = { nx - dirX * 80.0f - perpX * 30.0f, ny - dirY * 80.0f - perpY * 30.0f, nz - 30.0f, nx, ny, nz + 30.0f };
			break;
		case kAngle_HighAngle:
			//high angle looking down at NPC, camera in front and high
			candidates[0] = { nx - dirX * 60.0f + perpX * 30.0f, ny - dirY * 60.0f + perpY * 30.0f, nz + 80.0f, nx, ny, nz };
			candidates[1] = { nx - dirX * 60.0f - perpX * 30.0f, ny - dirY * 60.0f - perpY * 30.0f, nz + 80.0f, nx, ny, nz };
			break;
		case kAngle_PlayerFace:
			//close-up on player from side, looking at NPC (shows player facing their target)
			candidates[0] = { px + perpX * 80.0f, py + perpY * 80.0f, pz + 30.0f, nx, ny, nz };
			candidates[1] = { px - perpX * 80.0f, py - perpY * 80.0f, pz + 30.0f, nx, ny, nz };
			break;
		case kAngle_WideShot:
			//far back showing both characters and environment
			candidates[0] = { midX + perpX * 200.0f, midY + perpY * 200.0f, midZ + 60.0f, midX, midY, midZ };
			candidates[1] = { midX - perpX * 200.0f, midY - perpY * 200.0f, midZ + 60.0f, midX, midY, midZ };
			break;
		case kAngle_NPCProfile:
			//side profile of NPC
			candidates[0] = { nx + perpX * 90.0f, ny + perpY * 90.0f, nz + 40.0f, nx, ny, nz + 15.0f };
			candidates[1] = { nx - perpX * 90.0f, ny - perpY * 90.0f, nz + 40.0f, nx, ny, nz + 15.0f };
			break;
		case kAngle_PlayerProfile:
			//side profile of player
			candidates[0] = { px + perpX * 90.0f, py + perpY * 90.0f, pz + 40.0f, px, py, pz + 15.0f };
			candidates[1] = { px - perpX * 90.0f, py - perpY * 90.0f, pz + 40.0f, px, py, pz + 15.0f };
			break;
		case kAngle_Overhead:
			//above looking down at both
			candidates[0] = { midX, midY, midZ + 150.0f, midX, midY, midZ - 20.0f };
			candidates[1] = candidates[0];
			break;
		default:
			return;
	}

	//find best position using collision detection
	int chosen = 0;
	float bestClipFrac = 0.0f;

	for (int i = 0; i < 2; i++) {
		float clipFrac = CheckCameraClip(candidates[i].x, candidates[i].y, candidates[i].z,
										 candidates[i].lookX, candidates[i].lookY, candidates[i].lookZ,
										 g_dialogueTarget, player);
		Log("Candidate %d: clipFrac=%.3f", i, clipFrac);

		if (clipFrac > bestClipFrac) {
			bestClipFrac = clipFrac;
			chosen = i;
		}

		if (clipFrac >= 0.95f) {
			Log("Candidate %d is clear (no clipping)", i);
			break;
		}
	}

	float camX = candidates[chosen].x;
	float camY = candidates[chosen].y;
	float camZ = candidates[chosen].z;
	float lookX = candidates[chosen].lookX;
	float lookY = candidates[chosen].lookY;
	float lookZ = candidates[chosen].lookZ;

	//if camera clips, pull it back toward the look target
	float finalClipFrac = CheckCameraClip(camX, camY, camZ, lookX, lookY, lookZ, g_dialogueTarget, player);
	if (finalClipFrac < 0.95f) {
		Log("Adjusting camera: clip detected, pulling back by factor %.3f", finalClipFrac);
		//interpolate camera position toward look target
		camX = lookX + (camX - lookX) * finalClipFrac;
		camY = lookY + (camY - lookY) * finalClipFrac;
		camZ = lookZ + (camZ - lookZ) * finalClipFrac;
		Log("Adjusted camera position: (%.1f, %.1f, %.1f)", camX, camY, camZ);
	}

	//store base position for noise
	g_baseCamX = camX;
	g_baseCamY = camY;
	g_baseCamZ = camZ;
	g_baseLookX = lookX;
	g_baseLookY = lookY;
	g_baseLookZ = lookZ;

	//calculate rotation to look at target
	float toDirX = lookX - camX;
	float toDirY = lookY - camY;
	float toDirZ = lookZ - camZ;
	float horizDist = sqrtf(toDirX*toDirX + toDirY*toDirY);

	//yaw: angle in XY plane (game uses Y-forward)
	float yaw = atan2f(toDirX, toDirY) * (180.0f / PI);
	//pitch: angle up/down
	float pitch = -atan2f(toDirZ, horizDist) * (180.0f / PI);

	static const char* angleNames[] = {
		"Vanilla", "OverShoulder", "NPCCloseup", "TwoShot", "NPCFace", "LowAngle",
		"HighAngle", "PlayerFace", "WideShot", "NPCProfile", "PlayerProfile", "Overhead"
	};
	Log("Angle %d (%s): cam=(%.1f,%.1f,%.1f) look=(%.1f,%.1f,%.1f) yaw=%.1f pitch=%.1f",
		angle, angleNames[angle], camX, camY, camZ, lookX, lookY, lookZ, yaw, pitch);

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "print \"[Camera: %s]\"", angleNames[angle]);
	g_console->RunScriptLine(cmd, nullptr);
	snprintf(cmd, sizeof(cmd), "SetCameraTranslate 1 %.2f %.2f %.2f", camX, camY, camZ);
	g_console->RunScriptLine(cmd, nullptr);

	//apply rotation: reset all axes then set pitch and yaw
	Log("Setting rotation: axis0=0 axis1=%.2f axis2=0 axis3=%.2f", pitch, yaw);
	g_console->RunScriptLine("SetCameraRotate 1 0 0", nullptr);
	snprintf(cmd, sizeof(cmd), "SetCameraRotate 1 1 %.2f", pitch);
	g_console->RunScriptLine(cmd, nullptr);
	g_console->RunScriptLine("SetCameraRotate 1 2 0", nullptr);
	snprintf(cmd, sizeof(cmd), "SetCameraRotate 1 3 %.2f", yaw);
	g_console->RunScriptLine(cmd, nullptr);
}

static void ApplyCameraNoise() {
	if (!g_console) return;

	//rotation noise
	constexpr float kRotAmp = 3.0f;

	//base rotation to look at target
	float toDirX = g_baseLookX - g_baseCamX;
	float toDirY = g_baseLookY - g_baseCamY;
	float toDirZ = g_baseLookZ - g_baseCamZ;
	float horizDist = sqrtf(toDirX*toDirX + toDirY*toDirY);

	float yaw = atan2f(toDirX, toDirY) * (180.0f / PI);
	float pitch = -atan2f(toDirZ, horizDist) * (180.0f / PI);

	//smooth breathing motion
	yaw += (float)g_perlinYaw.octave1D(g_noiseTime, 3, 0.5) * kRotAmp;
	pitch += (float)g_perlinPitch.octave1D(g_noiseTime, 3, 0.5) * kRotAmp * 0.5f;

	char cmd[128];
	//must set all 4 axes like ApplyCameraAngle does
	g_console->RunScriptLine("SetCameraRotate 1 0 0", nullptr);
	snprintf(cmd, sizeof(cmd), "SetCameraRotate 1 1 %.4f", pitch);
	g_console->RunScriptLine(cmd, nullptr);
	g_console->RunScriptLine("SetCameraRotate 1 2 0", nullptr);
	snprintf(cmd, sizeof(cmd), "SetCameraRotate 1 3 %.4f", yaw);
	g_console->RunScriptLine(cmd, nullptr);
}

static void OnDialogueStart() {
	Log("OnDialogueStart called");

	InterfaceManager* intfc = InterfaceManager::GetSingleton();
	Log("InterfaceManager=%p", intfc);
	if (!intfc) return;

	g_dialogueTarget = intfc->crosshairRef;
	Log("crosshairRef=%p", g_dialogueTarget);
	if (!g_dialogueTarget) return;

	//must be an actor (Character=0x3B or Creature=0x3C), not a container/door/etc
	UInt8 typeID = g_dialogueTarget->typeID;
	if (typeID != 0x3B && typeID != 0x3C) {
		Log("crosshairRef is not an actor (typeID=%d), aborting camera", typeID);
		g_dialogueTarget = nullptr;
		return;
	}

	//check JohnnyGuitar is loaded
	HMODULE jg = GetModuleHandleA("JohnnyGuitar.dll");
	Log("JohnnyGuitar=%p", jg);
	if (!jg) return;

	PlayerCharacter* player = *g_thePlayer;
	Log("player=%p", player);
	if (!player) return;

	//0x650 = bThirdPerson flag (0=first, 1=third)
	g_wasFirstPerson = !*(bool*)((UInt8*)player + 0x650);
	Log("wasFirstPerson=%d", g_wasFirstPerson);

	//switch to 3rd person if in 1st
	if (g_wasFirstPerson) {
		Log("Switching to 3rd person");
		SetFirstPerson(player, false);
	}

	//force idle animation
	ForcePlayerIdle(player);

	g_currentAngle = kAngle_Vanilla;
	g_dialogueLineCount = 0;
	g_lastTopicInfoID = 0;
	g_cameraActive = true;

	ApplyCameraAngle(g_currentAngle);
	Log("OnDialogueStart complete");
}

static void OnDialogueEnd() {
	if (g_cameraActive) {
		DisableCamera();
		//restore 1st person if was in 1st
		if (g_wasFirstPerson) {
			PlayerCharacter* player = *g_thePlayer;
			if (player) {
				Log("Restoring 1st person");
				SetFirstPerson(player, true);
			}
		}
		g_cameraActive = false;
	}
	g_dialogueTarget = nullptr;
}

static int g_logThrottle = 0;

void Update() {
	bool dialogueMenuVisible = g_MenuVisibilityArray[kMenuType_Dialogue] != 0;

	if (g_logThrottle++ % 60 == 0) {
		Log("Update: menuVisible=%d inDialogue=%d cameraActive=%d", dialogueMenuVisible, g_inDialogue, g_cameraActive);
	}

	if (dialogueMenuVisible && !g_inDialogue) {
		Log("Dialogue started!");
		g_inDialogue = true;
		OnDialogueStart();
	}
	else if (!dialogueMenuVisible && g_inDialogue) {
		Log("Dialogue ended!");
		g_inDialogue = false;
		OnDialogueEnd();
	}

	if (!g_cameraActive || !g_dialogueTarget) return;

	//keep forcing idle every frame
	PlayerCharacter* player = *g_thePlayer;
	if (player) ForcePlayerIdle(player);

	//check if dialogue has advanced by monitoring currentInfo at offset 0x48
	DialogueMenu* dlgMenu = DialogueMenu::GetSingleton();
	if (dlgMenu) {
		UInt32 currentInfoAddr = (UInt32)dlgMenu->currentInfo;
		if (currentInfoAddr != 0 && currentInfoAddr != g_lastTopicInfoID) {
			g_lastTopicInfoID = currentInfoAddr;
			g_dialogueLineCount++;
			g_currentAngle = (CameraAngle)(g_dialogueLineCount % kAngle_Count);
			Log("Dialogue line %d (info=%08X), switching to angle %d", g_dialogueLineCount, currentInfoAddr, g_currentAngle);
			ApplyCameraAngle(g_currentAngle);
		}
	}

	//subtle camera noise - fixed increment per frame for consistent speed
	g_noiseTime += 0.005; //faster for testing
	ApplyCameraNoise();
}

bool Init(NVSEConsoleInterface* console) {
	g_console = console;
	fopen_s(&g_log, "DialogueCameraHandler.log", "w");
	Log("Init called, console=%p", console);

	//force 3rd person in dialogue (from RealTimeMenus)
	PatchMemoryNop(0x953124, 5);
	PatchMemoryNop(0x761DEF, 5);
	WriteRelJump(0x953ABF, 0x953AF5);
	WriteRelJump(0x762E55, 0x762E72);
	Log("Patched dialogue to force 3rd person");

	//disable dialogue zoom
	WriteRelJump(0x9533BE, 0x953562);
	SafeWrite8(0x953BBA, 0xEB);
	Log("Patched dialogue to disable zoom");

	//keep FocusOnActor for head tracking

	return g_console != nullptr;
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
