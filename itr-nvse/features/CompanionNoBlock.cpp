#include "CompanionNoBlock.h"

#include "internal/Detours.h"
#include "internal/GameLayout.h"
#include "internal/GameGlobals.h"
#include "internal/HavokLayout.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

extern void Log(const char* fmt, ...);

namespace
{
	typedef void* (__thiscall* _getRoot)(void*);
	const _getRoot getRoot = reinterpret_cast<_getRoot>(0x624020); //hkpCollidable::getRoot

	constexpr UInt32 kStalePairSteps = 600;
	constexpr UInt32 kRebuildInterval = 60; //proxy map only changes on cell/teammate change - no need to rebuild every frame

	bool g_enabled = false;
	volatile bool g_active = false; //read by the physics hook before the lock - volatile so it isn't cached
	bool g_interiorOnly = true;
	bool g_hookInstalled = false;
	volatile LONG g_releaseFrames = 30; //written on the main thread, read by the physics hook
	volatile LONG g_stepCounter = 0; //InterlockedIncrement on the main thread; snapshotted by the physics hook
	UInt32 g_rebuildCountdown = 0;

	CRITICAL_SECTION g_lock;
	bool g_lockInit = false;

	void* g_playerProxy = nullptr;
	void* g_playerPhantom = nullptr;
	std::unordered_set<void*> g_teammateProxies;
	std::unordered_set<void*> g_teammatePhantoms;
	std::unordered_map<void*, void*> g_proxyToPhantom; //pair key lookup on the teammate side

	struct PairContact { UInt32 count; UInt32 lastStep; };
	std::unordered_map<void*, PairContact> g_contactDur; //keyed by teammate phantom

	//owning hkpWorldObject of a contact collidable; hkpCollidable is embedded at hkpWorldObject+0x10
	void* ResolveCollidableToWorldObj(void* collidable)
	{
		if (!collidable)
			return nullptr;
		void* root = getRoot(collidable);
		return HkpWorldObjectFromCollidableRoot(root);
	}

	BhkCharacterControllerView* GetActorController(Actor* actor)
	{
		if (!actor)
			return nullptr;
		BaseProcess* process = actor->baseProcess;
		if (!process)
			return nullptr;
		if (process->processLevel > 1)
			return nullptr;
		return reinterpret_cast<BhkCharacterControllerView*>(
			reinterpret_cast<ProcessControllerView*>(process)->characterController);
	}

	void* GetActorProxy(BhkCharacterControllerView* ctrl)
	{
		return ctrl ? ctrl->proxy.serializable.hkObject : nullptr;
	}

	void* GetActorPhantom(BhkCharacterControllerView* ctrl)
	{
		if (!ctrl)
			return nullptr;
		BhkCharacterPhantomView* chrPhantom = ctrl->characterPhantom;
		return chrPhantom ? chrPhantom->havokPhantom : nullptr;
	}

	//advances at most once per step per key so both collectors and both proxy sides of a
	//pair agree within a frame; a gap in steps means contact broke, so the count resets
	bool PairShouldGhost(void* key)
	{
		auto it = g_contactDur.find(key);
		if (it == g_contactDur.end())
			return false;

		const UInt32 step = static_cast<UInt32>(g_stepCounter);
		const UInt32 release = static_cast<UInt32>(g_releaseFrames);
		PairContact& e = it->second;
		if (e.lastStep != step)
		{
			e.count = (e.lastStep == step - 1) ? e.count + 1 : 1;
			e.lastStep = step;
		}
		return e.count > release;
	}

	void FilterCollector(void* collector, bool proxyIsPlayer, void* selfPhantom)
	{
		if (!collector)
			return;
		auto* collectorView = reinterpret_cast<HkpAllCdPointCollectorView*>(collector);
		HkpRootCdPointView* hits = collectorView->hits.data;
		if (!hits || collectorView->hits.size <= 0)
			return;

		int n = collectorView->hits.size;
		for (int i = 0; i < n; )
		{
			HkpRootCdPointView* pt = hits + i;
			void* otherColl = pt->rootCollidableB;
			void* otherWO = ResolveCollidableToWorldObj(otherColl);

			void* pairKey = nullptr;
			if (proxyIsPlayer)
			{
				if (otherWO && g_teammatePhantoms.count(otherWO) != 0)
					pairKey = otherWO;
			}
			else if (otherWO && otherWO == g_playerPhantom)
			{
				pairKey = selfPhantom;
			}

			if (pairKey && PairShouldGhost(pairKey))
			{
				--n;
				if (i != n)
					memcpy(pt, hits + n, sizeof(HkpRootCdPointView));
			}
			else
			{
				++i;
			}
		}
		collectorView->hits.size = n;
	}

	Detours::JumpDetour g_updateManifoldDetour;
	typedef void (__thiscall* _updateManifold)(void*, void*, void*, bool);

	void __fastcall Hook_UpdateManifold(void* proxy, void*, void* startCollector, void* castCollector, bool isMultithreaded)
	{
		if (g_active && proxy)
		{
			EnterCriticalSection(&g_lock);
			bool proxyIsPlayer = (proxy == g_playerProxy && g_playerProxy != nullptr);
			bool proxyIsTeammate = !proxyIsPlayer && g_teammateProxies.count(proxy) != 0;
			if (proxyIsPlayer || proxyIsTeammate)
			{
				void* selfPhantom = nullptr;
				if (proxyIsTeammate)
				{
					auto it = g_proxyToPhantom.find(proxy);
					if (it != g_proxyToPhantom.end())
						selfPhantom = it->second;
				}
				FilterCollector(startCollector, proxyIsPlayer, selfPhantom);
				FilterCollector(castCollector, proxyIsPlayer, selfPhantom);
			}
			LeaveCriticalSection(&g_lock);
		}

		g_updateManifoldDetour.GetTrampoline<_updateManifold>()(proxy, startCollector, castCollector, isMultithreaded);
	}

	void InstallHook()
	{
		if (g_hookInstalled)
			return;

		//hkpCharacterProxy::updateManifold: push ebp; mov ebp,esp; and esp,0FFFFFFF0h (6 bytes)
		static const UInt8 kPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
		if (memcmp(reinterpret_cast<void*>(0xCAF4D0), kPrologue, sizeof(kPrologue)) != 0)
		{
			Log("CompanionNoBlock: unexpected bytes at 0xCAF4D0, not installing");
			return;
		}

		if (g_updateManifoldDetour.WriteRelJump(0xCAF4D0, Hook_UpdateManifold, 6))
			g_hookInstalled = true;
	}

	void RebuildProxyMap()
	{
		void* playerProxy = nullptr;
		void* playerPhantom = nullptr;
		std::unordered_set<void*> teammateProxies;
		std::unordered_set<void*> teammatePhantoms;
		std::unordered_map<void*, void*> proxyToPhantom;

		PlayerCharacter* player = *g_thePlayerPtr;
		bool gateOpen = false;
		if (player && player->parentCell && player->refID)
		{
			gateOpen = !g_interiorOnly || player->parentCell->IsInterior();
			BhkCharacterControllerView* playerCtrl = GetActorController(player);
			playerProxy = GetActorProxy(playerCtrl);
			playerPhantom = GetActorPhantom(playerCtrl);
			if (playerProxy && playerPhantom)
				proxyToPhantom[playerProxy] = playerPhantom;

			for (auto iter = player->teammates.Begin(); !iter.End(); ++iter)
			{
				Actor* actor = iter.Get();
				if (!actor || actor == player || !actor->refID)
					continue;
				if (actor->parentCell != player->parentCell)
					continue;
				BhkCharacterControllerView* ctrl = GetActorController(actor);
				void* proxy = GetActorProxy(ctrl);
				void* phantom = GetActorPhantom(ctrl);
				if (proxy)
					teammateProxies.insert(proxy);
				if (phantom)
					teammatePhantoms.insert(phantom);
				if (proxy && phantom)
					proxyToPhantom[proxy] = phantom;
			}
		}

		EnterCriticalSection(&g_lock);
		g_playerProxy = playerProxy;
		g_playerPhantom = playerPhantom;
		g_teammateProxies.swap(teammateProxies);
		g_teammatePhantoms.swap(teammatePhantoms);
		g_proxyToPhantom.swap(proxyToPhantom);
		const UInt32 step = static_cast<UInt32>(g_stepCounter);
		g_contactDur.reserve(g_teammatePhantoms.size());
		for (auto it = g_contactDur.begin(); it != g_contactDur.end(); )
		{
			if (g_teammatePhantoms.count(it->first) == 0 || step - it->second.lastStep > kStalePairSteps)
				it = g_contactDur.erase(it);
			else
				++it;
		}
		for (auto phantom : g_teammatePhantoms)
			if (g_contactDur.find(phantom) == g_contactDur.end())
				g_contactDur.emplace(phantom, PairContact{ 0, 0 });
		g_active = g_enabled && gateOpen && playerProxy != nullptr && !g_teammateProxies.empty();
		LeaveCriticalSection(&g_lock);
	}
}

namespace CompanionNoBlock
{
	void Init(bool enabled, int releaseFrames, int, bool interiorOnly)
	{
		if (!g_lockInit)
		{
			InitializeCriticalSection(&g_lock);
			g_lockInit = true;
		}
		UpdateSettings(enabled, releaseFrames, 0, interiorOnly);
		if (g_enabled)
			InstallHook();
	}

	void UpdateSettings(bool enabled, int releaseFrames, int, bool interiorOnly)
	{
		g_enabled = enabled;
		g_releaseFrames = releaseFrames > 0 ? releaseFrames : 1;
		g_interiorOnly = interiorOnly;

		if (!enabled)
			ClearState();
		else
			InstallHook();
	}

	void Update()
	{
		if (!g_enabled || !g_lockInit)
			return;

		InterlockedIncrement(&g_stepCounter);
		if (g_rebuildCountdown == 0)
		{
			RebuildProxyMap();
			g_rebuildCountdown = kRebuildInterval;
		}
		else
		{
			--g_rebuildCountdown;
		}
	}

	void ClearState()
	{
		if (!g_lockInit)
			return;
		g_rebuildCountdown = 0; //rebuild immediately on next enable
		EnterCriticalSection(&g_lock);
		g_active = false;
		g_playerProxy = nullptr;
		g_playerPhantom = nullptr;
		g_teammateProxies.clear();
		g_teammatePhantoms.clear();
		g_proxyToPhantom.clear();
		g_contactDur.clear();
		LeaveCriticalSection(&g_lock);
	}
}
