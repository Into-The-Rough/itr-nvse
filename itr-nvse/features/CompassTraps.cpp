//shows nearby mines and activator traps on the compass
//NOT hot-reloadable - requires game restart

#include "CompassTraps.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

extern void Log(const char* fmt, ...);

template <typename T_Ret = uint32_t, typename ...Args>
__forceinline T_Ret CT_ThisCall(uint32_t _addr, const void* _this, Args ...args) {
	return ((T_Ret(__thiscall*)(const void*, Args...))_addr)(_this, std::forward<Args>(args)...);
}

struct CT_String {
	char* m_data;
	UInt16 m_dataLen;
	UInt16 m_bufLen;
};

struct CT_TileValue {
	UInt32 id;
	void* parent;
	float num;
	char* str;
	void* action;

	void SetFloat(float value, bool propagate = true) {
		CT_ThisCall<void>(0xA0A270, this, value, propagate);
	}
};

struct CT_Tile;

//DListNode<Tile> - the iterator IS a DListNode where next points to current node
struct CT_TileListNode {
	CT_TileListNode* next;
	CT_TileListNode* prev;
	CT_Tile* data;

	CT_Tile* GetAndAdvance() {
		CT_Tile* item = nullptr;
		if (next) {
			item = next->data;
			next = next->next;
		}
		return item;
	}
};

struct CT_Tile {
	void* vtbl;
	void* childFirst;  //DList children - first node
	void* childLast;   //DList children - last node
	UInt32 childCount; //DList children - count
	void* valuesData;  //BSSimpleArray values - pBuffer
	UInt32 valuesSize;
	UInt32 valuesCapacity;
	UInt8 pad1C[4];
	CT_String name;
	CT_Tile* parent;
	void* node;
	UInt32 flags;
	UInt8 unk34;
	UInt8 unk35;
	UInt8 pad36[2];

	CT_TileValue* GetValue(UInt32 id) {
		if (!valuesData) return nullptr;
		return CT_ThisCall<CT_TileValue*>(0xA00E90, this, id);
	}

	bool HasValue(UInt32 id) {
		if (!valuesData) return false;
		return GetValue(id) != nullptr;
	}

	void SetVisible(bool visible, bool propagate) {
		if (!valuesData) return;
		CT_TileValue* val = CT_ThisCall<CT_TileValue*>(0xA01000, this, 0xFA3);
		if (val) val->SetFloat(visible ? 1.0f : 0.0f, propagate);
	}

	void SetFloat(UInt32 id, float value, bool propagate = true) {
		if (!valuesData) return;
		CT_TileValue* val = CT_ThisCall<CT_TileValue*>(0xA01000, this, id);
		if (val) val->SetFloat(value, propagate);
	}

	void SetString(UInt32 id, const char* str) {
		if (!valuesData) return;
		CT_ThisCall(0xA01350, this, id, str, false);
	}

	bool IsCompassMarker() {
		//compass markers have the filename (0xFCC) and visible (0xFA3) values
		return valuesData && HasValue(0xFCC) && HasValue(0xFA3);
	}
};

struct CT_TESModel {
	void* vtbl;
	CT_String nifPath;
};

struct CT_TESModelTextureSwap : CT_TESModel {
	UInt8 pad[0x10];
};

struct CT_TESObjectACTI {
	void* vtbl;
	UInt32 typeID;
	UInt32 flags;
	UInt32 refID;
	UInt8 pad14[0x28];
	CT_TESModelTextureSwap modelTextureSwap; //0x3C
};

struct CT_BGSProjectile {
	void* vtbl;
	UInt32 typeID;
	UInt32 flags;
	UInt32 refID;
	UInt8 pad14[0x4C];
	UInt16 projFlags; //0x60
};

struct CT_Projectile {
	void* vtbl;
	UInt32 typeID;
	UInt32 flags;
	UInt32 refID;
	UInt8 pad10[0x28];
	float posX; //0x38
	float posY;
	float posZ;
	UInt8 pad44[0x40];
	void* baseForm; //0x84
	UInt8 pad88[0x40];
	UInt32 projFlags; //0xC8
};

template <typename T>
struct CT_tList {
	struct Node {
		T* data;
		Node* next;
	};
	Node* head;

	struct Iterator {
		Node* node;
		Iterator(Node* n) : node(n) {}
		bool End() const { return !node; }
		void Next() { node = node->next; }
		T* Get() const { return node ? node->data : nullptr; }
	};

	Iterator Begin() const { return Iterator(head); }
};

struct CT_TESObjectCELL {
	void* vtbl;
	UInt32 typeID;
	UInt32 flags;
	UInt32 refID;
	UInt8 pad10[0x9C];
	CT_tList<TESObjectREFR> objectList; //0xAC

	bool IsInterior() { return CT_ThisCall<bool>(0x546FF0, this); }
};

struct CT_HUDMainMenu {
	void* vtbl;
	void* rootTile;
	UInt8 pad08[0x20C];
	UInt32 compassWidth;    //0x214
	UInt32 maxCompassAngle; //0x218

	static CT_HUDMainMenu* GetSingleton() { return *(CT_HUDMainMenu**)0x11D96C0; }
};

constexpr UInt32 kTileValue_visible = 0xFA3;
constexpr UInt32 kTileValue_alpha = 0xFA9;
constexpr UInt32 kTileValue_red = 0xFB2;
constexpr UInt32 kTileValue_green = 0xFB3;
constexpr UInt32 kTileValue_blue = 0xFB4;
constexpr UInt32 kTileValue_filename = 0xFCC;
constexpr UInt32 kTileValue_systemcolor = 0xFF4;

constexpr UInt8 kFormType_TESObjectACTI = 0x15;
constexpr UInt8 kFormType_GrenadeProjectile = 0x3E;

constexpr UInt16 kBaseProjFlag_AltTrigger = 0x4;
constexpr UInt16 kBaseProjFlag_CanBeDisabled = 0x20;
constexpr UInt16 kBaseProjFlag_Detonates = 0x400;
constexpr UInt16 kBaseProjFlag_Explosion = 0x2;

constexpr UInt32 kProjFlag_Disarmed = 0x200;

constexpr double kDblPI = 3.14159265358979323846;
constexpr double kDblPIx2 = 6.28318530717958647692;

//chain target for compass hook (Stewie's hook or original game code)
static UInt32 g_compassChainTarget = 0;

namespace CompassTraps
{
	constexpr UInt32 kMaxTraps = 32;

	bool g_bShowMines = true;
	bool g_bShowTraps = true;
	float g_fMaxTrapDistance = 1000.0f;
	float g_fMaxTrapDistanceSqr = 1000000.0f;
	UInt8 g_iMineColorR = 255;
	UInt8 g_iMineColorG = 50;
	UInt8 g_iMineColorB = 0;
	UInt8 g_iTrapColorR = 255;
	UInt8 g_iTrapColorG = 100;
	UInt8 g_iTrapColorB = 0;

	struct TrapRef {
		TESObjectREFR* ref;
		bool isMine;
	};

	TrapRef s_nearbyTraps[kMaxTraps];
	UInt32 s_trapCount = 0;

	UInt32 s_frameCounter = 30; //start at interval so first call triggers refresh
	constexpr UInt32 kRefreshInterval = 30; //refresh every 30 frames (~0.5s at 60fps)

	PlayerCharacter** g_thePlayer = (PlayerCharacter**)0x11DEA3C;

	bool IsMineArmed(TESObjectREFR* ref) {
		if ((ref->typeID & 0xFF) != kFormType_GrenadeProjectile) return false;

		CT_Projectile* proj = (CT_Projectile*)ref;
		CT_BGSProjectile* baseProj = (CT_BGSProjectile*)proj->baseForm;
		if (!baseProj) return false;

		UInt16 required = kBaseProjFlag_AltTrigger | kBaseProjFlag_CanBeDisabled | kBaseProjFlag_Explosion;
		UInt16 mask = required | kBaseProjFlag_Detonates;
		if ((baseProj->projFlags & mask) != required) return false;

		if (proj->projFlags & kProjFlag_Disarmed) return false;

		return true;
	}

	bool IsActivatorTrap(TESObjectREFR* ref) {
		if (!ref->baseForm) return false;
		if ((ref->baseForm->typeID & 0xFF) != kFormType_TESObjectACTI) return false;

		CT_TESObjectACTI* acti = (CT_TESObjectACTI*)ref->baseForm;
		const char* modelPath = acti->modelTextureSwap.nifPath.m_data;
		if (!modelPath) return false;

		return (strstr(modelPath, "Traps\\") != nullptr || strstr(modelPath, "Traps/") != nullptr);
	}

	void RefreshNearbyTraps() {
		s_trapCount = 0;

		PlayerCharacter* player = *g_thePlayer;
		if (!player) return;

		CT_TESObjectCELL* cell = (CT_TESObjectCELL*)player->parentCell;
		if (!cell) return;

		float playerX = player->posX;
		float playerY = player->posY;
		float playerZ = player->posZ;

		for (auto iter = cell->objectList.Begin(); !iter.End(); iter.Next()) {
			TESObjectREFR* ref = iter.Get();
			if (!ref) continue;

			//skip disabled (0x800), deleted (0x20), initially disabled (0x40)
			if (ref->IsDeleted() || (ref->flags & 0x840)) continue;

			float dx = ref->posX - playerX;
			float dy = ref->posY - playerY;
			float dz = ref->posZ - playerZ;
			float distSqr = dx*dx + dy*dy + dz*dz;
			if (distSqr > g_fMaxTrapDistanceSqr) continue;

			bool isMine = g_bShowMines && IsMineArmed(ref);
			bool isActivatorTrap = g_bShowTraps && !isMine && IsActivatorTrap(ref);

			if (isMine || isActivatorTrap) {
				if (s_trapCount < kMaxTraps) {
					s_nearbyTraps[s_trapCount].ref = ref;
					s_nearbyTraps[s_trapCount].isMine = isMine;
					s_trapCount++;
				}
			}
		}
	}

	double GetAngleBetweenPoints(float refX, float refY, float playerX, float playerY, float offset) {
		float dx = refX - playerX;
		float dy = refY - playerY;

		double angle = atan2(dx, dy) - offset;
		if (angle > -kDblPI) {
			if (angle > kDblPI) {
				angle = kDblPIx2 - angle;
			}
		} else {
			angle = kDblPIx2 + angle;
		}
		return angle;
	}

	double Remap(float outputMin, float outputMax, float min, float max, float input) {
		return (input - min) / (max - min) * (outputMax - outputMin) + outputMin;
	}

	static bool s_loggedOnce = false;

	void __fastcall RenderNearbyTraps(CT_TileListNode* tileIter, float* pHudOpacity) {
		//periodic refresh
		s_frameCounter++;
		if (s_frameCounter >= kRefreshInterval) {
			s_frameCounter = 0;
			RefreshNearbyTraps();
			if (!s_loggedOnce) {
				Log("CompassTraps: First refresh, found %d traps", s_trapCount);
				s_loggedOnce = true;
			}
		}

		if (s_trapCount == 0) return;
		if (!tileIter || !tileIter->next) return;

		CT_HUDMainMenu* hud = CT_HUDMainMenu::GetSingleton();
		if (!hud) return;

		PlayerCharacter* pc = *g_thePlayer;
		if (!pc) return;

		float playerX = pc->posX;
		float playerY = pc->posY;
		float playerZ = pc->posZ;
		float playerRotation = CT_ThisCall<float>(0x8BE960, pc, 0); //AdjustRot

		float hudOpacity = *pHudOpacity;
		float maxCompassAngle = (float)hud->maxCompassAngle;
		float maxWidth = (float)hud->compassWidth * 2.0f;
		UInt32 headingTrait = *(UInt32*)0x11D9E60;

		for (UInt32 i = 0; i < s_trapCount; i++) {
			TrapRef& trap = s_nearbyTraps[i];
			TESObjectREFR* ref = trap.ref;
			if (!ref) continue;

			float dx = ref->posX - playerX;
			float dy = ref->posY - playerY;
			float dz = ref->posZ - playerZ;
			float distSqr = dx*dx + dy*dy + dz*dz;
			if (distSqr > g_fMaxTrapDistanceSqr) continue;

			//get next available tile
			CT_Tile* tile = tileIter->GetAndAdvance();
			if (!tile || (UInt32)tile < 0x10000) return;
			if (!tile->valuesData) continue;

			double angle = GetAngleBetweenPoints(ref->posX, ref->posY, playerX, playerY, playerRotation) * 1.45;

			if (maxCompassAngle <= fabs(angle)) {
				tile->SetVisible(false, false);
				continue;
			}

			tile->SetString(kTileValue_filename, "Interface\\HUD\\glow_hud_tick_mark.dds");

			tile->SetFloat(kTileValue_systemcolor, 0);
			if (trap.isMine) {
				tile->SetFloat(kTileValue_red, g_iMineColorR);
				tile->SetFloat(kTileValue_green, g_iMineColorG);
				tile->SetFloat(kTileValue_blue, g_iMineColorB);
			} else {
				tile->SetFloat(kTileValue_red, g_iTrapColorR);
				tile->SetFloat(kTileValue_green, g_iTrapColorG);
				tile->SetFloat(kTileValue_blue, g_iTrapColorB);
			}

			tile->SetVisible(true, false);

			float width = (float)Remap(0, maxWidth, -maxCompassAngle, maxCompassAngle, (float)angle);
			tile->SetFloat(headingTrait, 2.0f + width, true);

			float dist = sqrtf(distSqr);
			float alpha = hudOpacity * (1.0f - dist / g_fMaxTrapDistance);
			tile->SetFloat(kTileValue_alpha, alpha, true);
		}
	}

	//periodic refresh in render function instead of cell change hook

	__declspec(naked) void PostCompassLocationsHook() {
		__asm {
			lea ecx, [ebp - 0x58]
			lea edx, [ebp - 0x20]
			push [g_compassChainTarget]
			jmp RenderNearbyTraps
		}
	}

	void __fastcall TileSetVisibleAndSystemColorOne(CT_Tile* tile, void* edx, int tileValueVisible, int _one) {
		tile->SetFloat(tileValueVisible, 1);
		tile->SetFloat(kTileValue_systemcolor, 1);
	}

	void WriteRelJump(UInt32 src, UInt32 dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE9;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void WriteRelCall(UInt32 src, UInt32 dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE8;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	UInt32 GetRelJeTarget(UInt32 src) {
		//check if it's a near je (0F 84 rel32)
		if (*(UInt16*)src == 0x840F) {
			return src + 6 + *(UInt32*)(src + 2);
		}
		//check if it's a short je (74 rel8)
		if (*(UInt8*)src == 0x74) {
			return src + 2 + *(SInt8*)(src + 1);
		}
		return 0;
	}

	void WriteRelJe(UInt32 src, UInt32 dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt16*)src = 0x840F;
		*(UInt32*)(src + 2) = dst - src - 6;
		VirtualProtect((void*)src, 6, oldProtect, &oldProtect);
	}


	void SafeWrite8(UInt32 addr, UInt8 data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void Init() {
		//get existing jump target (might be Stewie's hook or original 0x77984A)
		g_compassChainTarget = GetRelJeTarget(0x779359);
		if (!g_compassChainTarget) {
			Log("CompassTraps: Failed to read existing je target at 0x779359");
			return;
		}
		Log("CompassTraps: Chaining to existing target 0x%X", g_compassChainTarget);

		//compass render hooks - hook both locations
		WriteRelJe(0x779359, (UInt32)PostCompassLocationsHook);
		WriteRelJe(0x77936A, (UInt32)PostCompassLocationsHook);

		//reset system color for vanilla tiles
		WriteRelCall(0x779788, (UInt32)TileSetVisibleAndSystemColorOne);

		//increase max compass markers
		SafeWrite8(0x76CB44, 25);

		Log("CompassTraps: Initialized (mines=%d, traps=%d, distance=%.0f)",
			g_bShowMines, g_bShowTraps, g_fMaxTrapDistance);
		Log("CompassTraps: Mine color %d,%d,%d, Trap color %d,%d,%d",
			g_iMineColorR, g_iMineColorG, g_iMineColorB,
			g_iTrapColorR, g_iTrapColorG, g_iTrapColorB);
	}
}

void CompassTraps_Init(bool showMines, bool showTraps, float maxDistance,
	int mineR, int mineG, int mineB,
	int trapR, int trapG, int trapB)
{
	Log("CompassTraps: DISABLED for testing");
	return; //disabled for testing
	CompassTraps::g_bShowMines = showMines;
	CompassTraps::g_bShowTraps = showTraps;
	CompassTraps::g_fMaxTrapDistance = maxDistance;
	CompassTraps::g_fMaxTrapDistanceSqr = maxDistance * maxDistance;
	CompassTraps::g_iMineColorR = (UInt8)mineR;
	CompassTraps::g_iMineColorG = (UInt8)mineG;
	CompassTraps::g_iMineColorB = (UInt8)mineB;
	CompassTraps::g_iTrapColorR = (UInt8)trapR;
	CompassTraps::g_iTrapColorG = (UInt8)trapG;
	CompassTraps::g_iTrapColorB = (UInt8)trapB;
	CompassTraps::Init();
}
