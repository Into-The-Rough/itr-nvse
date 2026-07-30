#include "OnFootContactHandler.h"

#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/FootContactLogic.h"
#include "internal/GameSDK.h"
#include "internal/HavokLayout.h"
#include "internal/NiLayout.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/RayCast.h"
#include "internal/ScopedLock.h"
#include "internal/globals.h"

#include <Windows.h>
#include <cmath>

namespace
{
	constexpr const char* kFootstepEvent = "ITR:OnActorFootstep";
	constexpr const char* kFootContactEvent = "ITR:OnActorFootContact";
	constexpr UInt32 kPendingCapacity = 64;
	constexpr float kRayStartHeight = 24.0f;
	constexpr float kRayDepth = 96.0f;
	constexpr UInt8 kProjectileLayer = 6;

	struct PendingFootstep
	{
		UInt32 refID;
		SInt32 soundID;
	};

	struct Contact
	{
		FootContactLogic::Point3 position;
		FootContactLogic::Point3 normal;
		SInt32 material;
		UInt8 layer;
	};

	Detours::CallDetour s_footstepDetour;
	EventDispatch::ListenerProbe s_footstepProbe = {
		kFootstepEvent, "ITR_OnActorFootstepProbe",
		[](TESObjectREFR*, void*) {}
	};
	EventDispatch::ListenerProbe s_contactProbe = {
		kFootContactEvent, "ITR_OnActorFootContactProbe",
		[](TESObjectREFR*, void*) {}
	};
	CRITICAL_SECTION s_queueLock;
	volatile LONG s_queueLockInit = 0;
	PendingFootstep s_pending[kPendingCapacity] = {};
	UInt32 s_pendingCount = 0;
	UInt32 s_droppedCount = 0;
	DWORD s_mainThreadID = 0;
	bool s_hookInstalled = false;
	bool s_eventsReady = false;
	thread_local bool s_inDispatch = false;

	bool IsActorRef(TESObjectREFR* ref)
	{
		if (!ref)
			return false;
		return ref->typeID == kFormType_ACHR || ref->typeID == kFormType_ACRE;
	}

	bool GetFootPosition(Actor* actor, int side, FootContactLogic::Point3& position)
	{
		if (!actor || side == FootContactLogic::kSide_Unknown)
			return false;

		//getninode returns the active player root; foot contacts need the third-person skeleton
		NiNode* root = reinterpret_cast<NiNode*(__thiscall*)(TESObjectREFR*, bool)>(0x950BB0)(actor, false);
		if (!root)
			return false;

		const char* boneName = side == FootContactLogic::kSide_Right
			? "Bip01 R Foot"
			: "Bip01 L Foot";
		void* foot = reinterpret_cast<void*(__cdecl*)(void*, const char*)>(0x4AAE30)(root, boneName);
		if (!foot)
			return false;

		const float* translate = NiAVObjectAsView(foot)->world.translate;
		position = { translate[0], translate[1], translate[2] };
		return FootContactLogic::IsFinite(position);
	}

	bool GetActorCollisionGroup(Actor* actor, UInt16& group)
	{
		if (!actor || !actor->baseProcess || actor->baseProcess->processLevel > 1)
			return false;

		auto* process = reinterpret_cast<ProcessControllerView*>(actor->baseProcess);
		auto* controller = reinterpret_cast<BhkCharacterControllerView*>(process->characterController);
		if (!controller || !controller->characterPhantom)
			return false;

		void* phantom = controller->characterPhantom->havokPhantom;
		if (!phantom)
			return false;

		const UInt32 filterInfo = HkpWorldObjectGetCollisionFilterInfo(phantom);
		group = static_cast<UInt16>(filterInfo >> 16);
		return group != 0;
	}

	bool CastGround(const FootContactLogic::Point3& footPosition, UInt16 group, Contact& contact)
	{
		const FootContactLogic::Point3 from = {
			footPosition.x,
			footPosition.y,
			footPosition.z + kRayStartHeight,
		};
		const FootContactLogic::Point3 to = {
			footPosition.x,
			footPosition.y,
			footPosition.z - kRayDepth,
		};

		RayCastData ray = {};
		ray.pos0[0] = from.x * kHavokScale;
		ray.pos0[1] = from.y * kHavokScale;
		ray.pos0[2] = from.z * kHavokScale;
		ray.pos1[0] = to.x * kHavokScale;
		ray.pos1[1] = to.y * kHavokScale;
		ray.pos1[2] = to.z * kHavokScale;
		ray.hitFraction = 1.0f;
		ray.unk44[0] = 0xFFFFFFFF;
		ray.unk44[6] = 0xFFFFFFFF;
		ray.layerType = kProjectileLayer;
		ray.group = group;

		if (!Engine::TESPickObject(&ray, true) || ray.budgetSpent ||
			!ray.cdBody || !std::isfinite(ray.hitFraction) || ray.hitFraction >= 1.0f)
		{
			return false;
		}

		contact.position = FootContactLogic::Interpolate(from, to, ray.hitFraction);
		contact.normal = { ray.normal[0], ray.normal[1], ray.normal[2] };
		if (!FootContactLogic::Normalize(contact.normal))
			contact.normal = { 0.0f, 0.0f, 1.0f };

		const UInt32 filterInfo = HkpCollidableGetCollisionFilterInfo(ray.cdBody);
		contact.layer = static_cast<UInt8>(filterInfo & 0x7F);
		const UInt32 material = reinterpret_cast<UInt32(__cdecl*)(void*, FootContactLogic::Point3*)>(0x62B150)(
			ray.cdBody, &contact.position);
		contact.material = material < 36 ? static_cast<SInt32>(material) : -1;
		return true;
	}

	void DispatchFootstep(Actor* actor, SInt32 soundID, int side,
		const FootContactLogic::Point3& position)
	{
		g_eventManagerInterface->DispatchEvent(
			kFootstepEvent, actor,
			static_cast<TESForm*>(actor),
			soundID,
			side,
			PackEventFloatArg(position.x),
			PackEventFloatArg(position.y),
			PackEventFloatArg(position.z));
	}

	void DispatchContact(Actor* actor, SInt32 soundID, int side, const Contact& contact)
	{
		g_eventManagerInterface->DispatchEvent(
			kFootContactEvent, actor,
			static_cast<TESForm*>(actor),
			soundID,
			side,
			PackEventFloatArg(contact.position.x),
			PackEventFloatArg(contact.position.y),
			PackEventFloatArg(contact.position.z),
			PackEventFloatArg(contact.normal.x),
			PackEventFloatArg(contact.normal.y),
			PackEventFloatArg(contact.normal.z),
			contact.material,
			static_cast<int>(contact.layer));
	}

	struct DispatchGuard
	{
		DispatchGuard() { s_inDispatch = true; }
		~DispatchGuard() { s_inDispatch = false; }
	};

	void ProcessFootstep(Actor* actor, SInt32 soundID)
	{
		if (!actor || !g_eventManagerInterface || g_isLoadingSave || s_inDispatch)
			return;

		const bool wantsFootstep = s_footstepProbe.hasListeners != 0;
		const bool wantsContact = s_contactProbe.hasListeners != 0;
		if (!wantsFootstep && !wantsContact)
			return;

		DispatchGuard guard;
		const UInt32 refID = actor->refID;
		const int side = FootContactLogic::ResolveSide(soundID);
		FootContactLogic::Point3 footPosition = { actor->posX, actor->posY, actor->posZ };
		const bool hasFootPosition = GetFootPosition(actor, side, footPosition);

		Contact contact = {};
		bool hasContact = false;
		if (wantsContact && hasFootPosition && actor->typeID == kFormType_ACHR)
		{
			UInt16 group = 0;
			if (GetActorCollisionGroup(actor, group))
				hasContact = CastGround(footPosition, group, contact);
		}

		if (wantsFootstep)
			DispatchFootstep(actor, soundID, side, footPosition);

		if (hasContact)
		{
			if (wantsFootstep)
			{
				actor = static_cast<Actor*>(Engine::LookupFormByID(refID));
				if (!actor || actor->typeID != kFormType_ACHR || actor->refID != refID)
					return;
			}

			DispatchContact(actor, soundID, side, contact);
		}
	}

	void QueueFootstep(TESObjectREFR* actor, SInt32 soundID)
	{
		if (!actor || s_queueLockInit != 2)
			return;

		ScopedLock lock(&s_queueLock);
		if (s_pendingCount >= kPendingCapacity)
		{
			++s_droppedCount;
			return;
		}
		s_pending[s_pendingCount++] = { actor->refID, soundID };
	}

	UInt32 DrainPending(PendingFootstep* local, UInt32& dropped)
	{
		ScopedLock lock(&s_queueLock);
		const UInt32 count = s_pendingCount;
		for (UInt32 i = 0; i < count; ++i)
			local[i] = s_pending[i];
		s_pendingCount = 0;
		dropped = s_droppedCount;
		s_droppedCount = 0;
		return count;
	}

	using PlayFootstep_t = void (__cdecl*)(TESObjectREFR*, int);

	void __cdecl PlayFootstepHook(TESObjectREFR* actor, int soundID)
	{
		reinterpret_cast<PlayFootstep_t>(s_footstepDetour.GetOverwrittenAddr())(actor, soundID);

		if (!s_eventsReady || !IsActorRef(actor) ||
			(!s_footstepProbe.hasListeners && !s_contactProbe.hasListeners))
		{
			return;
		}

		if (GetCurrentThreadId() != s_mainThreadID)
		{
			QueueFootstep(actor, soundID);
			return;
		}

		ProcessFootstep(static_cast<Actor*>(actor), soundID);
	}
}

namespace OnFootContactHandler
{
	bool Init(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		if (nvse->isEditor)
			return false;

		InitCriticalSectionOnce(&s_queueLockInit, &s_queueLock);
		s_mainThreadID = GetCurrentThreadId();

		if (!s_footstepDetour.WriteRelCall(0x5F2F93, reinterpret_cast<UInt32>(PlayFootstepHook)))
		{
			Log("OnFootContactHandler failed: 0x5F2F93 is not an E8 call");
			return false;
		}

		s_hookInstalled = true;
		return true;
	}

	void InstallListenerProbes()
	{
		s_footstepProbe.Install();
		s_contactProbe.Install();
		s_eventsReady = s_hookInstalled && g_eventManagerInterface != nullptr;
	}

	void Update()
	{
		if (!s_eventsReady || !g_eventManagerInterface)
			return;

		s_footstepProbe.Refresh(false);
		s_contactProbe.Refresh(false);

		PendingFootstep local[kPendingCapacity];
		UInt32 dropped = 0;
		const UInt32 count = DrainPending(local, dropped);
		if (dropped)
			Log("OnFootContactHandler dropped %u off-thread footsteps", dropped);

		for (UInt32 i = 0; i < count; ++i)
		{
			auto* actor = static_cast<Actor*>(Engine::LookupFormByID(local[i].refID));
			if (IsActorRef(actor) && actor->refID == local[i].refID)
				ProcessFootstep(actor, local[i].soundID);
		}
	}

	void ClearState()
	{
		if (s_queueLockInit == 2)
		{
			ScopedLock lock(&s_queueLock);
			s_pendingCount = 0;
			s_droppedCount = 0;
		}
		s_inDispatch = false;
	}
}
