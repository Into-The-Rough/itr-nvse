#include "CompanionNoBlock.h"

#include "internal/Detours.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

extern void Log(const char* fmt, ...);

namespace
{
	PlayerCharacter** g_player = reinterpret_cast<PlayerCharacter**>(0x11DEA3C);

	typedef void* (__thiscall* _getRoot)(void*);
	const _getRoot getRoot = reinterpret_cast<_getRoot>(0x624020); //hkpCollidable::getRoot

	//hkpCharacterProxy::updateManifold reads both collectors as
	//hkpAllCdPointCollector::m_hits hkArray at +0x10: data +0x00, size +0x04.
	constexpr UInt32 kCollector_HitsData = 0x10;
	constexpr UInt32 kCollector_HitsSize = 0x14;
	//the same function copies hkpRootCdPoint in 112-byte chunks and compares
	//m_rootCollidableB at +0x48 while matching old manifold points.
	constexpr UInt32 kRootCdPointStride = 112;
	constexpr UInt32 kRootCdPoint_CollidableB = 0x48;
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

	//owning hkpWorldObject of a contact collidable, hkpCollidable is embedded at hkpWorldObject+0x10
	void* ResolveCollidableToWorldObj(void* collidable)
	{
		if (!collidable)
			return nullptr;
		void* root = getRoot(collidable);
		if (!root)
			return nullptr;
		return static_cast<UInt8*>(root) - 0x10;
	}

	void* GetActorController(void* actor)
	{
		if (!actor)
			return nullptr;
		void* process = *reinterpret_cast<void**>(static_cast<UInt8*>(actor) + 0x68);
		if (!process)
			return nullptr;
		if (*reinterpret_cast<UInt32*>(static_cast<UInt8*>(process) + 0x28) > 1)
			return nullptr;
		return *reinterpret_cast<void**>(static_cast<UInt8*>(process) + 0x138);
	}

	void* GetActorProxy(void* ctrl)
	{
		return ctrl ? *reinterpret_cast<void**>(static_cast<UInt8*>(ctrl) + 0x08) : nullptr;
	}

	//the actor's character-controller havok phantom, i.e. the thing other proxies collide
	//against. chain matches OnContactHandler::MapActorPhantom: *( *(ctrl+0x594) + 0x08 )
	void* GetActorPhantom(void* ctrl)
	{
		if (!ctrl)
			return nullptr;
		void* chrPhantom = *reinterpret_cast<void**>(static_cast<UInt8*>(ctrl) + 0x594);
		if (!chrPhantom)
			return nullptr;
		return *reinterpret_cast<void**>(static_cast<UInt8*>(chrPhantom) + 0x08);
	}

	//advances at most once per step per key so both collectors and both proxy sides of a
	//pair agree within a frame - a gap in steps means contact broke, so the count resets
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
		UInt8* base = static_cast<UInt8*>(collector);
		UInt8** data = reinterpret_cast<UInt8**>(base + kCollector_HitsData);
		int* size = reinterpret_cast<int*>(base + kCollector_HitsSize);
		UInt8* hits = *data;
		if (!hits || *size <= 0)
			return;

		int n = *size;
		for (int i = 0; i < n; )
		{
			UInt8* pt = hits + static_cast<UInt32>(i) * kRootCdPointStride;
			void* otherColl = *reinterpret_cast<void**>(pt + kRootCdPoint_CollidableB);
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
					memcpy(pt, hits + static_cast<UInt32>(n) * kRootCdPointStride, kRootCdPointStride);
			}
			else
			{
				++i;
			}
		}
		*size = n;
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

		PlayerCharacter* player = g_player ? *g_player : nullptr;
		bool gateOpen = false;
		if (player && player->parentCell && player->refID)
		{
			gateOpen = !g_interiorOnly || player->parentCell->IsInterior();
			void* playerCtrl = GetActorController(player);
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
				void* ctrl = GetActorController(actor);
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
