
#include "OnNearMissHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/EngineFunctions.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/NearMissTracking.h"
#include "internal/globals.h"
#include "internal/settings.h"
#include <Windows.h>
#include <math.h>
#include <unordered_map>
#include <vector>

constexpr UInt32 kAddr_ProjectileUpdate = 0x9BECC0;

constexpr UInt32 kOff_NearestActors = 0x88;
constexpr UInt32 kOff_NearestCount  = 0x150;
constexpr UInt32 kMaxNearestActors  = 50;

constexpr UInt32 kRefr_Position   = 0x30;

constexpr float kActorBodyHeight = 128.0f;

//Actor.uiLifeState - only these two stand and react, the rest are dead/dying/downed
constexpr UInt32 kLifeState_Alive      = 0;
constexpr UInt32 kLifeState_Restrained = 5; //engine Actor::IsRestrained compares lifeState == 5

using Vec3 = NearMissTracking::Point3;

//projectile update runs on the main thread but inside the mobile-object iteration, so we
//queue refIDs here and dispatch from the main loop to keep scripts off the live list
struct QueuedNearMiss { UInt32 victimID; UInt32 shooterID; UInt32 weaponID; float dist; };

typedef int (__thiscall* ProjectileUpdate_t)(void*, int);

static Detours::JumpDetour s_detour;
static std::unordered_map<UInt64, UInt32> s_lastFire;
static std::vector<QueuedNearMiss> s_pending;
static NearMissTracking::FlightHistory s_flights;
static UInt32 s_generation = 0;
static bool s_dispatching = false;

static void NearMissProbe(TESObjectREFR*, void*) {}
static EventDispatch::ListenerProbe s_probe = { "ITR:OnNearMiss", "ITR_OnNearMissProbe", NearMissProbe };
static volatile UInt8 s_hasListeners = 1;

static Vec3 GetRefPos(TESObjectREFR* ref)
{
	return {ref->posX, ref->posY, ref->posZ};
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

//true if the round already logged a collision with this actor this frame - it hit rather
//than passed. the move populates impactDataList before this scan runs
static bool ProjectileHitActor(ProjectileView* proj, Actor* actor)
{
	for (auto it = proj->impacts.Begin(); !it.End(); ++it)
		if (*it && it->ref == actor) return true;
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

static bool PassesCooldown(TESObjectREFR* shooter, TESObjectREFR* victim, UInt32 now) {
	if (!shooter || !victim) return false;
	UInt64 key = ((UInt64)shooter->refID << 32) | victim->refID;
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

static void ScanFlight(ProjectileView* proj, TESObjectREFR* shooter, TESObjectWEAP* weapon, const Vec3& a, const Vec3& b)
{
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

	auto test = [&](Actor* actor) {
		if (!actor || actor == shooter) return;
		const UInt32 ls = actor->lifeState;
		if (ls != kLifeState_Alive && ls != kLifeState_Restrained) return; //corpses and downed actors do not near-miss

		const float* p = (const float*)((UInt8*)actor + kRefr_Position);
		//where along this frame's sweep the actor's column lies - outside [0,1] is another frame
		const float midz = p[2] + kActorBodyHeight * 0.5f;
		const float u = ((p[0]-a.x)*abx + (p[1]-a.y)*aby + (midz-a.z)*abz) / seg2;
		if (u < 0.0f || u > 1.0f) return;

		Vec3 feet = { p[0], p[1], p[2] };
		Vec3 head = { p[0], p[1], p[2] + kActorBodyHeight };
		const float d2 = SegSegDistSq(a, b, feet, head);
		if (d2 > r2 || ProjectileHitActor(proj, actor) || !PassesCooldown(shooter, actor, now)) return;

		s_pending.push_back({
			actor->refID,
			shooter ? shooter->refID : 0,
			weapon ? weapon->refID : 0,
			sqrtf(d2)
		});
	};

	for (UInt32 i = 0; i < count; ++i)
		test(static_cast<Actor*>(nearest[i]));
	//the nearest list is ranked by distance to the player, so the player is never in it
	test(*g_thePlayerPtr);
}

namespace OnNearMissHandler {
	static int __fastcall HookProjectileUpdate(TESObjectREFR* ref, void*, int a2)
	{
		auto original = s_detour.GetTrampoline<ProjectileUpdate_t>();
		if (!s_hasListeners || !g_eventManagerInterface || g_isLoadingSave)
			return original(ref, a2);
		auto* proj = reinterpret_cast<ProjectileView*>(ref);
		if (!TESFormIsActorRef(proj->sourceRef) || !TESFormIsWeapon(proj->sourceWeap))
			return original(ref, a2);

		const UInt32 refID = ref->refID;
		const UInt32 shooterID = proj->sourceRef->refID;
		const UInt32 weaponID = proj->sourceWeap->refID;
		const UInt32 generation = s_generation;
		const bool hitscan = (proj->flags & kProjectileRefFlag_Hitscan) != 0;
		const float lifeBefore = proj->lifeTime;
		const Vec3 before = GetRefPos(ref);
		const NiVector3 direction = proj->vector;
		const int ret = original(ref, a2);

		//impact callbacks can delete forms or clear state during the original update
		if (generation != s_generation || Engine::LookupFormByID(refID) != ref) return ret;
		auto* shooter = static_cast<TESObjectREFR*>(Engine::LookupFormByID(shooterID));
		auto* weapon = static_cast<TESObjectWEAP*>(Engine::LookupFormByID(weaponID));
		if (!TESFormIsActorRef(shooter) || !TESFormIsWeapon(weapon)) return ret;

		Vec3 a = before;
		Vec3 b = GetRefPos(ref);
		if (hitscan) {
			if (lifeBefore > 0.0f) return ret;
			const float length = sqrtf(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
			if (length < 0.001f) return ret;
			const float dnx = direction.x / length;
			const float dny = direction.y / length;
			const float dnz = direction.z / length;
			float range = proj->range;
			if (range < 256.0f) range = 256.0f;
			if (range > 8192.0f) range = 8192.0f;
			for (auto it = proj->impacts.Begin(); !it.End(); ++it) {
				if (!*it) continue;
				const auto& pos = it->pos;
				const float t = (pos.x - a.x) * dnx + (pos.y - a.y) * dny + (pos.z - a.z) * dnz;
				if (t >= 0.0f && t < range) range = t;
			}
			if (range <= 0.0f) return ret;
			b = {a.x + dnx * range, a.y + dny * range, a.z + dnz * range};
		} else {
			//grenade havok motion occurs between updates and never sets vector104 to its displacement
			a = s_flights.Observe(refID, lifeBefore, proj->lifeTime, before, b);
		}
		ScanFlight(proj, shooter, weapon, a, b);
		return ret;
	}

	bool Init(void* nvseInterface)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
		if (nvse->isEditor) return false;

		const UInt8 prologue[] = {0x53, 0x8B, 0xDC, 0x51, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04};
		if (memcmp(reinterpret_cast<void*>(kAddr_ProjectileUpdate), prologue, sizeof(prologue))) {
			Log("OnNearMiss: projectile update prologue already patched, hook skipped");
			return false;
		}
		return s_detour.WriteRelJump(kAddr_ProjectileUpdate, HookProjectileUpdate, 10);
	}

	void ClearState()
	{
		++s_generation;
		s_flights.Clear();
		s_pending.clear();
		s_lastFire.clear();
	}

	void InstallListenerProbe()
	{
		s_probe.Install();
		s_hasListeners = 1;
	}

	void Update()
	{
		if (s_dispatching) return;
		const bool hadListeners = s_hasListeners != 0;
		s_hasListeners = s_probe.Refresh(false) ? 1 : 0;
		if (hadListeners && !s_hasListeners) ClearState();
		if (s_pending.empty() || !g_eventManagerInterface) {
			s_pending.clear();
			return;
		}
		std::vector<QueuedNearMiss> batch;
		batch.swap(s_pending);
		const UInt32 generation = s_generation;
		s_dispatching = true;
		for (const auto& e : batch) {
			if (generation != s_generation) break;
			TESForm* victim = (TESForm*)Engine::LookupFormByID(e.victimID);
			if (!TESFormIsActorRef(victim)) continue;
			TESForm* shooter = e.shooterID ? (TESForm*)Engine::LookupFormByID(e.shooterID) : nullptr;
			TESForm* weapon = e.weaponID ? (TESForm*)Engine::LookupFormByID(e.weaponID) : nullptr;
			if (!TESFormIsActorRef(shooter)) shooter = nullptr;
			if (!TESFormIsWeapon(weapon)) weapon = nullptr;
			g_eventManagerInterface->DispatchEvent("ITR:OnNearMiss", nullptr,
				victim, shooter, weapon, PackEventFloatArg(e.dist));
		}
		s_dispatching = false;
	}
}
