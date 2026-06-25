
#include "OnNearMissHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/settings.h"
#include <Windows.h>
#include <math.h>
#include <unordered_map>
#include <vector>

constexpr UInt32 kAddr_ProjectileUpdate = 0x9BECC0;

constexpr UInt32 kOff_NearestActors = 0x88;
constexpr UInt32 kOff_NearestCount  = 0x150;
constexpr UInt32 kMaxNearestActors  = 50;

constexpr UInt32 kProj_ImpactList = 0x88; //tList<ImpactData>, one entry per contact, struck ref at +0
constexpr UInt32 kRefr_Position   = 0x30;

constexpr float kActorBodyHeight = 128.0f;

struct Vec3 { float x, y, z; };

//projectile update runs on the main thread but inside the mobile-object iteration, so we
//queue here and dispatch from the main loop to keep scripts off the live list
struct QueuedNearMiss { TESForm* victim; TESForm* shooter; TESForm* weapon; float dist; };

typedef int (__thiscall* ProjectileUpdate_t)(void*, int);

static Detours::JumpDetour s_detour;
static std::unordered_map<UInt64, UInt32> s_lastFire;
static std::unordered_map<void*, Vec3> s_lastPos;
static std::vector<QueuedNearMiss> s_pending;

static void GetRefPos(void* ref, Vec3* out) {
	const float* p = (const float*)((UInt8*)ref + kRefr_Position);
	out->x = p[0]; out->y = p[1]; out->z = p[2];
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

//true if the round already logged a collision with this actor this frame - it hit rather
//than passed. the move populates impactDataList before this scan runs
static bool ProjectileHitActor(void* proj, void* actor) {
	for (void** node = (void**)((UInt8*)proj + kProj_ImpactList); node; node = (void**)node[1])
		if (node[0] && *(void**)node[0] == actor) return true;
	return false;
}

//squared distance between segment p1->q1 and segment p2->q2
static float SegSegDistSq(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
	const float d1x = q1.x-p1.x, d1y = q1.y-p1.y, d1z = q1.z-p1.z;
	const float d2x = q2.x-p2.x, d2y = q2.y-p2.y, d2z = q2.z-p2.z;
	const float rx  = p1.x-p2.x, ry  = p1.y-p2.y, rz  = p1.z-p2.z;
	const float a = d1x*d1x + d1y*d1y + d1z*d1z;
	const float e = d2x*d2x + d2y*d2y + d2z*d2z;
	const float f = d2x*rx + d2y*ry + d2z*rz;
	const float EPS = 1e-6f;

	float s, t;
	if (a <= EPS) {
		s = 0.0f;
		//bullet didn't move this frame, treat it as a point and find where on the actor's body (0 = feet, 1 = head) is closest
		t = e <= EPS ? 0.0f : Clamp01(f / e);
	} else {
		const float c = d1x*rx + d1y*ry + d1z*rz;
		if (e <= EPS) {
			t = 0.0f;
			s = Clamp01(-c / a);
		} else {
			const float b = d1x*d2x + d1y*d2y + d1z*d2z;
			const float denom = a*e - b*b;
			s = denom > EPS ? Clamp01((b*f - c*e) / denom) : 0.0f;
			t = (b*s + f) / e;
			if (t < 0.0f)      { t = 0.0f; s = Clamp01(-c / a); }
			else if (t > 1.0f) { t = 1.0f; s = Clamp01((b - c) / a); }
		}
	}

	const float cx = (p1.x + d1x*s) - (p2.x + d2x*t);
	const float cy = (p1.y + d1y*s) - (p2.y + d2y*t);
	const float cz = (p1.z + d1z*s) - (p2.z + d2z*t);
	return cx*cx + cy*cy + cz*cz;
}

static bool PassesCooldown(void* shooter, void* victim, UInt32 now) {
	UInt64 key = ((UInt64)(UInt32)shooter << 32) | (UInt32)victim;
	auto it = s_lastFire.find(key);
	if (it != s_lastFire.end() && (now - it->second) < (UInt32)Settings::iNearMissCooldownMs)
		return false;
	s_lastFire[key] = now;
	if (s_lastFire.size() > 256) {
		UInt32 stale = (UInt32)Settings::iNearMissCooldownMs * 4;
		for (auto i = s_lastFire.begin(); i != s_lastFire.end(); )
			i = (now - i->second) > stale ? s_lastFire.erase(i) : ++i;
	}
	return true;
}

static void ScanFlight(void* proj, TESObjectREFR* shooter, TESObjectWEAP* weapon, const Vec3& a, const Vec3& b) {
	const float radius = Settings::fNearMissRadius;
	if (radius <= 0.0f) return;
	const float r2 = radius * radius;

	const float abx = b.x-a.x, aby = b.y-a.y, abz = b.z-a.z;
	const float seg2 = abx*abx + aby*aby + abz*abz;
	if (seg2 <= 0.0f) return;

	UInt32 now = GetTickCount();
	void** nearest = (void**)((UInt8*)g_processManager + kOff_NearestActors);
	UInt32 count = *(UInt32*)((UInt8*)g_processManager + kOff_NearestCount);
	if (count > kMaxNearestActors) count = kMaxNearestActors;

	for (UInt32 i = 0; i < count; ++i) {
		void* actor = nearest[i];
		if (!actor || actor == shooter) continue;

		const float* p = (const float*)((UInt8*)actor + kRefr_Position);
		//where along this frame's sweep the actor's column lies - outside [0,1] is another frame
		const float midz = p[2] + kActorBodyHeight * 0.5f;
		const float u = ((p[0]-a.x)*abx + (p[1]-a.y)*aby + (midz-a.z)*abz) / seg2;
		if (u < 0.0f || u > 1.0f) continue;

		Vec3 feet = { p[0], p[1], p[2] };
		Vec3 head = { p[0], p[1], p[2] + kActorBodyHeight };
		const float d2 = SegSegDistSq(a, b, feet, head);
		if (d2 > r2 || ProjectileHitActor(proj, actor) || !PassesCooldown(shooter, actor, now)) continue;

		s_pending.push_back({
			(TESForm*)actor,
			(TESForm*)shooter,
			weapon,
			sqrtf(d2)
		});
	}
}

static int __fastcall HookProjectileUpdate(void* proj, void*, int a2) {
	TESObjectREFR* shooter = nullptr;
	TESObjectWEAP* weapon = nullptr;
	if (Settings::bOnNearMiss && g_eventManagerInterface) {
		shooter = ProjectileGetSourceRef(proj);
		weapon = ProjectileGetSourceWeapon(proj);
	}

	int ret = s_detour.GetTrampoline<ProjectileUpdate_t>()(proj, a2);

	if (!Settings::bOnNearMiss || !g_eventManagerInterface)
		return ret;

	if (!TESFormIsActorRef(shooter) || !TESFormIsWeapon(weapon))
		return ret;

	//the ref position settles once per frame, so build the swept segment from last frame's
	Vec3 cur;
	GetRefPos(proj, &cur);

	auto it = s_lastPos.find(proj);
	if (it == s_lastPos.end()) {
		if (s_lastPos.size() > 1024) s_lastPos.clear();
		s_lastPos[proj] = cur;
		return ret;
	}
	Vec3 prev = it->second;
	it->second = cur;

	const float dx = cur.x-prev.x, dy = cur.y-prev.y, dz = cur.z-prev.z;
	const float seg2 = dx*dx + dy*dy + dz*dz;
	if (seg2 < 1.0f || seg2 > 1.0e8f) return ret; //stationary, or a reused projectile slot

	ScanFlight(proj, shooter, weapon, prev, cur);
	return ret;
}

namespace OnNearMissHandler {
bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	//prologue: push ebx; mov ebx,esp; push ecx; and esp,~0xF; add esp,4 = 1+2+1+3+3 = 10
	return s_detour.WriteRelJump(kAddr_ProjectileUpdate, HookProjectileUpdate, 10);
}

void Update() {
	if (s_pending.empty() || !g_eventManagerInterface) {
		s_pending.clear();
		return;
	}
	std::vector<QueuedNearMiss> batch;
	batch.swap(s_pending);
	for (const auto& e : batch) {
		if (!e.victim) continue;
		g_eventManagerInterface->DispatchEvent("ITR:OnNearMiss", nullptr,
			e.victim, e.shooter, e.weapon, PackEventFloatArg(e.dist));
	}
}
}
