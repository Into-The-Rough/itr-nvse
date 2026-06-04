#include "VATSHighlightDepthFix.h"
#include "internal/SafeWrite.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/globals.h"

#include <Windows.h>
#include <cstring>

namespace VATSHighlightDepthFix
{
	static bool g_deferredHighlight = false;
	static bool g_forceSceneDepthTest = false;
	static bool g_loggedDepthPath = false;
	static bool g_loggedLiveDepthPath = false;
	static bool g_loggedDepthFallback = false;
	static bool g_loggedVATSCommandSuppressed = false;

	using TESMain_HandleVATSOcclusionQueries_t = void(__thiscall*)(void*, bool);
	using InterfaceManager_GetVATSHighlightData_t = void*(__cdecl*)();
	using VATSHighlightData_SetTarget_t = void(__thiscall*)(void*, void*, UInt32, bool);
	using VATSHighlightData_ResetRefs_t = void(__thiscall*)(void*);
	using VATSHighlightData_SetMode_t = void(__thiscall*)(void*, UInt32*);
	using DrawTargetToTexture_t = void(__thiscall*)(void*, void*, UInt32, bool, UInt32, UInt32, bool);
	using SetupImageSpace_t = void(__thiscall*)(void*, void*, void*, UInt32, bool);
	using ScriptCommand_t = bool(__cdecl*)(void*, void*, void*, void*, void*, void*, double*, UInt32*);

	struct NiColorA
	{
		float r;
		float g;
		float b;
		float a;
	};

	static auto TESMain_HandleVATSOcclusionQueries = reinterpret_cast<TESMain_HandleVATSOcclusionQueries_t>(0x874AC0);
	static auto InterfaceManager_GetVATSHighlightData = reinterpret_cast<InterfaceManager_GetVATSHighlightData_t>(0x705910);
	static auto VATSHighlightData_SetTarget = reinterpret_cast<VATSHighlightData_SetTarget_t>(0x800AC0);
	static auto VATSHighlightData_ResetRefs = reinterpret_cast<VATSHighlightData_ResetRefs_t>(0x800ED0);
	static auto VATSHighlightData_SetMode = reinterpret_cast<VATSHighlightData_SetMode_t>(0x44AC20);
	static auto DrawTargetToTexture = reinterpret_cast<DrawTargetToTexture_t>(0x800F30);
	static auto SetupImageSpace = reinterpret_cast<SetupImageSpace_t>(0x801A60);
	static Detours::JumpDetour s_highlightAdditionalReferenceDetour;
	static Detours::JumpDetour s_deactivateAllHighlightsDetour;
	static ScriptCommand_t s_highlightAdditionalReference = nullptr;
	static ScriptCommand_t s_deactivateAllHighlights = nullptr;

	UInt32 GetVATSMode()
	{
		return *reinterpret_cast<UInt32*>(0x11F2258);
	}

	void* GetVATSMenu()
	{
		return *reinterpret_cast<void**>(0x11DB0D4);
	}

	void* GetCurrentVATSTarget()
	{
		return *reinterpret_cast<void**>(0x11F21CC);
	}

	bool IsVanillaVATSOwnerActive()
	{
		return GetVATSMode() != 0 || GetVATSMenu() || GetCurrentVATSTarget();
	}

	void* GetVATSHighlightData()
	{
		return InterfaceManager_GetVATSHighlightData();
	}

	void* GetCurrentVATSTexture()
	{
		return *reinterpret_cast<void**>(0x11DEB38);
	}

	bool HasAdditionalRefs(void* vatsData)
	{
		if (!vatsData) return false;
		return *reinterpret_cast<UInt32*>(vatsData) != 0 &&
			*reinterpret_cast<SInt32*>(static_cast<UInt8*>(vatsData) + 0x0C) > 0;
	}

	void AddRefNiObject(void* object)
	{
		if (object)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<UInt8*>(object) + 4));
	}

	void ReleaseNiObject(void* object)
	{
		if (!object) return;
		if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<UInt8*>(object) + 4)) == 0)
		{
			auto vtbl = *reinterpret_cast<UInt32**>(object);
			reinterpret_cast<void(__thiscall*)(void*)>(vtbl[1])(object);
		}
	}

	void* GetRenderTargetGroup(void* renderedTexture)
	{
		return ThisCall<void*>(0xB6B260, renderedTexture);
	}

	void* GetDepthStencil(void* renderTargetGroup)
	{
		if (!renderTargetGroup) return nullptr;
		return *reinterpret_cast<void**>(static_cast<UInt8*>(renderTargetGroup) + 0x20);
	}

	UInt32 GetMSAAPref(void* rendererData)
	{
		if (!rendererData) return 0;
		return *reinterpret_cast<UInt32*>(static_cast<UInt8*>(rendererData) + 0x10);
	}

	bool IsDepthCompatible(void* targetGroup, void* depth)
	{
		if (!targetGroup || !depth) return false;

		UInt32 targetWidth = ThisCall<UInt32>(0xEE8490, targetGroup, 0);
		UInt32 targetHeight = ThisCall<UInt32>(0xEE84B0, targetGroup, 0);
		UInt32 depthWidth = *reinterpret_cast<UInt32*>(static_cast<UInt8*>(depth) + 0x08);
		UInt32 depthHeight = *reinterpret_cast<UInt32*>(static_cast<UInt8*>(depth) + 0x0C);
		if (!targetWidth || !targetHeight || depthWidth < targetWidth || depthHeight < targetHeight)
			return false;

		void* targetColorRendererData = ThisCall<void*>(0xEE86E0, targetGroup, 0);
		void* depthRendererData = *reinterpret_cast<void**>(static_cast<UInt8*>(depth) + 0x10);
		return GetMSAAPref(targetColorRendererData) == GetMSAAPref(depthRendererData);
	}

	void ForceDepthTestState()
	{
		CdeclCall(0xB97DE0, 1, 0);
		CdeclCall(0xB97E30, 0, 0);
		CdeclCall(0xB97E80, 3, 0);
	}

	void BeginTextureWithTransparentClear(void* renderer, void* renderTargetGroup)
	{
		NiColorA oldBackground;
		NiColorA transparentBlack = { 0.0f, 0.0f, 0.0f, 0.0f };

		ThisCall<void>(0xE759C0, renderer, &oldBackground);
		ThisCall<void>(0xE758F0, renderer, &transparentBlack);
		CdeclCall(0xB6B7D0, renderTargetGroup, 1);
		ThisCall<void>(0xE758F0, renderer, &oldBackground);
	}

	void __cdecl Hook_SetZEnable(UInt32 zEnable, UInt32 stateDelta)
	{
		if (g_forceSceneDepthTest && zEnable == 0)
			zEnable = 1;

		CdeclCall(0xB97DE0, zEnable, stateDelta);
	}

	void WriteRelCallOrLog(UInt32 hookSite, UInt32 hook, UInt32 expectedTarget, const char* name)
	{
		if (*reinterpret_cast<UInt8*>(hookSite) != 0xE8)
		{
			Log("VATSHighlightDepthFix: %s hook site at %08X is not a call (0x%02X)", name, hookSite, *reinterpret_cast<UInt8*>(hookSite));
			return;
		}

		const UInt32 target = hookSite + 5 + *reinterpret_cast<SInt32*>(hookSite + 1);
		if (target != expectedTarget)
		{
			Log("VATSHighlightDepthFix: %s hook site at %08X already retargeted to %08X (expected %08X); skipping",
				name, hookSite, target, expectedTarget);
			return;
		}

		SafeWrite::WriteRelCall(hookSite, hook);
	}

	void WriteScriptCommandJumpOrLog(Detours::JumpDetour& detour, UInt32 hookSite, UInt32 hook, ScriptCommand_t& original, const char* name)
	{
		//both command Execute fns share this prologue (FNV 1.4.0.525):
		//push ebp; mov ebp,esp; sub esp,14h - validate before detouring, JumpDetour only rejects E9.
		//default original to the real callee so a stray call is safe if we skip.
		static const UInt8 kPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 };
		original = reinterpret_cast<ScriptCommand_t>(hookSite);

		if (memcmp(reinterpret_cast<void*>(hookSite), kPrologue, sizeof(kPrologue)) != 0)
		{
			Log("VATSHighlightDepthFix: %s hook site at %08X has unexpected prologue; skipping", name, hookSite);
			return;
		}

		UInt8* trampoline = nullptr;
		if (detour.WriteRelJump(hookSite, hook, 6, &trampoline))
			original = reinterpret_cast<ScriptCommand_t>(trampoline);
		else
			Log("VATSHighlightDepthFix: %s hook site at %08X is already patched", name, hookSite);
	}

	void ResetHighlightDataForVATS(void* vatsData)
	{
		if (!vatsData) return;

		VATSHighlightData_SetTarget(vatsData, nullptr, 0, false);
		VATSHighlightData_ResetRefs(vatsData);
		UInt32 mode = 0;
		VATSHighlightData_SetMode(vatsData, &mode);
	}

	void RenderHighlightVanilla(void* vatsData, void* vatsTexture)
	{
		CdeclCall(0xB6B790);
		CdeclCall(0xB98380, 0, 1);
		DrawTargetToTexture(vatsData, vatsTexture, 0, false, 1, 7, false);
		CdeclCall(0x4ECED0, 1);
		DrawTargetToTexture(vatsData, vatsTexture, 0, false, 0, 1, true);
	}

	bool RenderHighlightWithSceneDepth(void* vatsData, void* sceneTexture, void* vatsTexture)
	{
		void* vatsGroup = GetRenderTargetGroup(vatsTexture);
		void* sceneGroup = GetRenderTargetGroup(sceneTexture);
		void* sceneDepth = GetDepthStencil(sceneGroup);
		if (!IsDepthCompatible(vatsGroup, sceneDepth))
		{
			if (!g_loggedDepthFallback)
			{
				g_loggedDepthFallback = true;
				Log("VATSHighlightDepthFix: scene depth unavailable or incompatible; using vanilla highlight path");
			}
			return false;
		}

		void* originalDepth = GetDepthStencil(vatsGroup);
		AddRefNiObject(originalDepth);

		CdeclCall(0xB6B790);

		ThisCall<bool>(0xEE8690, vatsGroup, sceneDepth);
		g_forceSceneDepthTest = true;
		ForceDepthTestState();
		DrawTargetToTexture(vatsData, vatsTexture, 0, false, 0, 1, true);
		g_forceSceneDepthTest = false;
		CdeclCall(0xB97DE0, 0, 0);
		ThisCall<bool>(0xEE8690, vatsGroup, originalDepth);

		ReleaseNiObject(originalDepth);
		if (!g_loggedDepthPath)
		{
			g_loggedDepthPath = true;
			Log("VATSHighlightDepthFix: rendered VATS highlight against scene depth");
		}
		return true;
	}

	bool RenderHighlightWithCurrentDepth(void* vatsData)
	{
		void* vatsTexture = GetCurrentVATSTexture();
		if (!vatsTexture)
			return false;

		void* renderer = CdeclCall<void*>(0x43C4B0);
		if (!renderer)
			return false;

		void* sceneGroup = ThisCall<void*>(0xE75810, renderer);
		void* sceneDepth = GetDepthStencil(sceneGroup);
		void* vatsGroup = GetRenderTargetGroup(vatsTexture);
		if (!IsDepthCompatible(vatsGroup, sceneDepth))
			return false;

		void* originalDepth = GetDepthStencil(vatsGroup);
		AddRefNiObject(originalDepth);

		ThisCall<bool>(0xEE8690, vatsGroup, sceneDepth);
		BeginTextureWithTransparentClear(renderer, vatsGroup);

		g_forceSceneDepthTest = true;
		ForceDepthTestState();
		DrawTargetToTexture(vatsData, vatsTexture, 0, false, 0, 0, false);
		g_forceSceneDepthTest = false;

		CdeclCall(0xB6B840);
		ThisCall<bool>(0xEE8690, vatsGroup, originalDepth);
		ReleaseNiObject(originalDepth);

		if (!g_loggedLiveDepthPath)
		{
			g_loggedLiveDepthPath = true;
			Log("VATSHighlightDepthFix: rendered VATS highlight against live world depth");
		}
		return true;
	}

	void __fastcall Hook_TESMain_HandleVATSOcclusionQueries(void* tesMain, void* edx, bool isInVATS)
	{
		if (IsVanillaVATSOwnerActive())
		{
			g_deferredHighlight = false;
			TESMain_HandleVATSOcclusionQueries(tesMain, isInVATS);
			return;
		}

		void* vatsData = GetVATSHighlightData();
		if (!isInVATS && HasAdditionalRefs(vatsData))
		{
			g_deferredHighlight = true;
			return;
		}

		if (!isInVATS)
			g_deferredHighlight = false;

		TESMain_HandleVATSOcclusionQueries(tesMain, isInVATS);
	}

	bool __cdecl Hook_HighlightAdditionalReference(void* paramInfo, void* scriptData, void* thisObj, void* containingObj, void* scriptObj, void* eventList, double* result, UInt32* opcodeOffsetPtr)
	{
		if (IsVanillaVATSOwnerActive())
		{
			if (!g_loggedVATSCommandSuppressed)
			{
				g_loggedVATSCommandSuppressed = true;
				Log("VATSHighlightDepthFix: suppressing script-driven VATS highlights while VATS is active mode=%u menu=%08X target=%08X",
					GetVATSMode(),
					reinterpret_cast<UInt32>(GetVATSMenu()),
					reinterpret_cast<UInt32>(GetCurrentVATSTarget()));
			}
			return true;
		}

		return s_highlightAdditionalReference(paramInfo, scriptData, thisObj, containingObj, scriptObj, eventList, result, opcodeOffsetPtr);
	}

	bool __cdecl Hook_DeactivateAllHighlights(void* paramInfo, void* scriptData, void* thisObj, void* containingObj, void* scriptObj, void* eventList, double* result, UInt32* opcodeOffsetPtr)
	{
		if (IsVanillaVATSOwnerActive())
			return true;

		return s_deactivateAllHighlights(paramInfo, scriptData, thisObj, containingObj, scriptObj, eventList, result, opcodeOffsetPtr);
	}

	void __fastcall Hook_VATSMenu_SetAdditionalRefMode(void* vatsData, void* edx, UInt32* mode)
	{
		//VATS target selection also uses additional refs, so clear script-owned refs before vanilla repopulates them
		ResetHighlightDataForVATS(vatsData);
		VATSHighlightData_SetMode(vatsData, mode);
	}

	void __fastcall Hook_SetupImageSpace(void* vatsData, void* edx, void* sceneTexture, void* vatsTexture, UInt32 scanTexture, bool abIsRenderedMenu)
	{
		if (g_deferredHighlight)
		{
			g_deferredHighlight = false;
			if (!RenderHighlightWithSceneDepth(vatsData, sceneTexture, vatsTexture))
				RenderHighlightVanilla(vatsData, vatsTexture);
		}

		SetupImageSpace(vatsData, sceneTexture, vatsTexture, scanTexture, abIsRenderedMenu);
	}

	void __cdecl Hook_RenderScenePostResolveDepth(void* camera, void* accumulator, UInt32 unknown)
	{
		if (g_deferredHighlight)
		{
			void* vatsData = GetVATSHighlightData();
			if (RenderHighlightWithCurrentDepth(vatsData))
				g_deferredHighlight = false;
		}

		CdeclCall(0xB6B930, camera, accumulator, unknown);
	}

	void Init()
	{
		WriteRelCallOrLog(0x870C63, reinterpret_cast<UInt32>(Hook_TESMain_HandleVATSOcclusionQueries), 0x874AC0, "TESMain::HandleVATSOcclusionQueries");
		WriteRelCallOrLog(0x8741DB, reinterpret_cast<UInt32>(Hook_RenderScenePostResolveDepth), 0xB6B930, "BSShaderUtil::RenderScenePostResolveDepth");
		WriteRelCallOrLog(0x8760A9, reinterpret_cast<UInt32>(Hook_SetupImageSpace), 0x801A60, "VATSHighlightData::SetupImageSpace");
		WriteRelCallOrLog(0xBC9C92, reinterpret_cast<UInt32>(Hook_SetZEnable), 0xB97DE0, "BSRenderState::SetZEnable");
		WriteRelCallOrLog(0x7F3E5D, reinterpret_cast<UInt32>(Hook_VATSMenu_SetAdditionalRefMode), 0x44AC20, "VATSMenu target-list highlight reset");
		WriteScriptCommandJumpOrLog(s_highlightAdditionalReferenceDetour, 0x5BB610, reinterpret_cast<UInt32>(Hook_HighlightAdditionalReference), s_highlightAdditionalReference, "Script::HighlightAdditionalReference");
		WriteScriptCommandJumpOrLog(s_deactivateAllHighlightsDetour, 0x5BB6C0, reinterpret_cast<UInt32>(Hook_DeactivateAllHighlights), s_deactivateAllHighlights, "Cmd_DeactivateAllHighlights");
	}
}
