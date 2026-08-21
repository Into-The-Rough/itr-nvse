//vanilla renders script-driven highlights before the world, so they come out x-ray
//defer to the scene pass after the depth resolve and borrow the scene depth-stencil

#include "VATSHighlightDepthFix.h"
#include "internal/SafeWrite.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/globals.h"
#include "internal/MenuLayout.h"
#include "internal/NiLayout.h"

#include <Windows.h>
#include <cstring>

namespace VATSHighlightDepthFix
{
	static bool g_deferredHighlight = false;
	static bool g_scriptHighlightsOwned = false;
	static bool g_forceSceneDepthTest = false;
	static bool g_loggedDepthPath = false;
	static bool g_loggedDepthFallback = false;

	//0x874AC0's arg is 0x8749B0's "query pass already ran", not a VATS state, and it goes unread
	using TESMain_HandleVATSOcclusionQueries_t = void(__thiscall*)(void*, UInt32);
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
	static Detours::CallDetour s_handleVATSOcclusionQueriesCall;
	static Detours::CallDetour s_renderScenePostResolveDepthCall;
	static Detours::CallDetour s_setupImageSpaceCall;
	static Detours::CallDetour s_setZEnableCall;
	static Detours::CallDetour s_setAdditionalRefModeCall;
	static Detours::JumpDetour s_highlightAdditionalReferenceDetour;
	static Detours::JumpDetour s_deactivateAllHighlightsDetour;
	static ScriptCommand_t s_highlightAdditionalReference = nullptr;
	static ScriptCommand_t s_deactivateAllHighlights = nullptr;

	UInt32 GetVATSMode()
	{
		return VATSGetMode();
	}

	void* GetVATSMenu()
	{
		return VATSGetMenu();
	}

	void* GetCurrentVATSTarget()
	{
		return VATSGetCurrentTarget();
	}

	//not the current target: VATSMenu::Close never clears 0x11F21CC, so it stays set after VATS
	//ends and would keep the engine looking busy for the rest of the session
	bool IsVanillaVATSOwnerActive()
	{
		return GetVATSMode() != 0 || GetVATSMenu();
	}

	void* GetVATSHighlightData()
	{
		return InterfaceManager_GetVATSHighlightData();
	}

	void* GetCurrentVATSTexture()
	{
		return VATSGetRenderedTexture();
	}

	//mode 0 draws the target, which ShowOff parks on the player outside VATS
	bool HasAdditionalRefs(void* vatsData)
	{
		return VATSHighlightDataHasRefs(vatsData);
	}

	bool IsRendererIdle()
	{
		return CdeclCall<bool>(0x4E9510);
	}

	//script-owned only, so vanilla keeps every VATS frame including the shot sequence
	bool ShouldDeferHighlight()
	{
		if (!g_scriptHighlightsOwned) return false;
		if (IsVanillaVATSOwnerActive()) return false;
		return HasAdditionalRefs(GetVATSHighlightData());
	}

	void AddRefNiObject(void* object)
	{
		if (object)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&NiRefObjectAsView(object)->refCount));
	}

	void ReleaseNiObject(void* object)
	{
		if (!object) return;
		if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(&NiRefObjectAsView(object)->refCount)) == 0)
		{
			auto vtbl = static_cast<UInt32*>(NiRefObjectAsView(object)->vtbl);
			reinterpret_cast<void(__thiscall*)(void*)>(vtbl[1])(object);
		}
	}

	void* GetRenderTargetGroup(void* renderedTexture)
	{
		return ThisCall<void*>(0xB6B260, renderedTexture);
	}

	void* GetDepthStencil(void* renderTargetGroup)
	{
		return NiRenderTargetGroupGetDepthStencil(renderTargetGroup);
	}

	UInt32 GetMSAAPref(void* rendererData)
	{
		return NiRenderTargetRendererDataGetMSAAPref(rendererData);
	}

	bool IsDepthCompatible(void* targetGroup, void* depth)
	{
		if (!targetGroup || !depth) return false;

		UInt32 targetWidth = ThisCall<UInt32>(0xEE8490, targetGroup, 0);
		UInt32 targetHeight = ThisCall<UInt32>(0xEE84B0, targetGroup, 0);
		UInt32 depthWidth = Ni2DBufferGetWidth(depth);
		UInt32 depthHeight = Ni2DBufferGetHeight(depth);
		if (!targetWidth || !targetHeight || depthWidth < targetWidth || depthHeight < targetHeight)
			return false;

		void* targetColorRendererData = ThisCall<void*>(0xEE86E0, targetGroup, 0);
		void* depthRendererData = Ni2DBufferGetRendererData(depth);
		return GetMSAAPref(targetColorRendererData) == GetMSAAPref(depthRendererData);
	}

	void ForceDepthTestState()
	{
		CdeclCall(0xB97DE0, 1, 0);
		CdeclCall(0xB97E30, 0, 0);
		CdeclCall(0xB97E80, 3, 0);
	}

	void __cdecl Hook_SetZEnable(UInt32 zEnable, UInt32 stateDelta)
	{
		if (g_forceSceneDepthTest && zEnable == 0)
			zEnable = 1;

		CdeclCall(0xB97DE0, zEnable, stateDelta);
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

	void WriteRelCallOrLog(Detours::CallDetour& detour, UInt32 hookSite, UInt32 hook, UInt32 expectedTarget, const char* name)
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

		if (!detour.WriteRelCall(hookSite, hook))
			Log("VATSHighlightDepthFix: %s hook site at %08X could not be detoured", name, hookSite);
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

	//clears the target too, not just the refs: mode 0 with an occupied target slot is what
	//vanilla renders through the limb-isolation path
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

	//mid-scene, so bind the target and pop only what we pushed, a vanilla-style drain would
	//drop the scene's own target. 0x800F30 pushes its own only while the renderer is idle
	bool RenderHighlightWithCurrentDepth(void* vatsData)
	{
		if (IsRendererIdle())
			return false;

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

		ThisCall<bool>(0xEE8690, vatsGroup, sceneDepth);
		BeginTextureWithTransparentClear(renderer, vatsGroup);

		g_forceSceneDepthTest = true;
		ForceDepthTestState();
		DrawTargetToTexture(vatsData, vatsTexture, 0, false, 0, 0, false);
		g_forceSceneDepthTest = false;
		//hand back both forced states, the site wanted ZEnable 0 and ZWrite off breaks later geometry
		CdeclCall(0xB97DE0, 0, 0);
		CdeclCall(0xB97E30, 1, 0);

		CdeclCall(0xB6B840);
		ThisCall<bool>(0xEE8690, vatsGroup, originalDepth);
		ReleaseNiObject(originalDepth);

		if (!g_loggedDepthPath)
		{
			g_loggedDepthPath = true;
			Log("VATSHighlightDepthFix: rendered VATS highlight against live scene depth");
		}
		return true;
	}

	//ShowOff parks the player in the target slot after the command returns, and its own
	//IsInVATS test misses the menu, so it clobbers the menu's target and mode 0 then draws the
	//player. put the menu's own target back, and if there isn't one, skip the render instead of
	//emptying the slot - the menu re-asserts its target on the next idle tick either way
	bool RepairParkedPlayerTarget()
	{
		auto* data = static_cast<VATSHighlightDataView*>(GetVATSHighlightData());
		if (!data || data->mode != 0 || !data->targetNode) return true;
		if (data->target != reinterpret_cast<TESObjectREFR*>(*g_thePlayerPtr)) return true;

		TESObjectREFR* menuTarget = reinterpret_cast<TESObjectREFR*>(GetCurrentVATSTarget());
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			Log("VATSHighlightDepthFix: player parked over the VATS target, menu target %s",
				menuTarget ? "restored" : "unavailable, skipping the render");
		}

		if (!menuTarget)
			return false;

		//+0x23C is the aimed part index, hand it back so the same limb stays selected
		const UInt32 part = *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(data) + 0x23C);
		VATSHighlightData_SetTarget(data, menuTarget, part, false);
		return true;
	}

	void __fastcall Hook_TESMain_HandleVATSOcclusionQueries(void* tesMain, void* edx, UInt32 queryPassRan)
	{
		if (IsVanillaVATSOwnerActive() && !RepairParkedPlayerTarget())
			return;

		//a deferral still standing means the scene pass never took it, so give the frame back
		if (!g_deferredHighlight && ShouldDeferHighlight())
		{
			g_deferredHighlight = true;
			return;
		}

		g_deferredHighlight = false;
		TESMain_HandleVATSOcclusionQueries(tesMain, queryPassRan);
	}

	void __cdecl Hook_RenderScenePostResolveDepth(void* camera, void* accumulator, UInt32 unknown)
	{
		if (g_deferredHighlight)
		{
			void* vatsData = GetVATSHighlightData();
			if (HasAdditionalRefs(vatsData) && RenderHighlightWithCurrentDepth(vatsData))
				g_deferredHighlight = false;
		}

		CdeclCall(0xB6B930, camera, accumulator, unknown);
	}

	//during VATS the engine owns the list outright, script refs would take the slots the menu
	//needs for its body part highlights. EvictParkedPlayerTarget cleans up after the callers
	//that write the target slot regardless of the command's answer
	bool __cdecl Hook_HighlightAdditionalReference(void* paramInfo, void* scriptData, void* thisObj, void* containingObj, void* scriptObj, void* eventList, double* result, UInt32* opcodeOffsetPtr)
	{
		if (IsVanillaVATSOwnerActive())
			return true;

		g_scriptHighlightsOwned = true;
		return s_highlightAdditionalReference(paramInfo, scriptData, thisObj, containingObj, scriptObj, eventList, result, opcodeOffsetPtr);
	}

	//the same rule as the add: while VATS is up the engine owns the data. letting this through
	//clears the menu's own target and its body part highlight with it
	bool __cdecl Hook_DeactivateAllHighlights(void* paramInfo, void* scriptData, void* thisObj, void* containingObj, void* scriptObj, void* eventList, double* result, UInt32* opcodeOffsetPtr)
	{
		if (IsVanillaVATSOwnerActive())
			return true;

		g_scriptHighlightsOwned = false;
		return s_deactivateAllHighlights(paramInfo, scriptData, thisObj, containingObj, scriptObj, eventList, result, opcodeOffsetPtr);
	}

	void __fastcall Hook_VATSMenu_SetAdditionalRefMode(void* vatsData, void* edx, UInt32* mode)
	{
		//VATS target selection also uses additional refs, so clear script-owned refs before vanilla
		//repopulates them. only when we own some: SetTarget also zeroes the aimed part index at
		//+0x572, which picks the branch for the engine's own hide/force-show pair
		//script refs saturate the 31 slot list, so free it or vanilla has nowhere to put the
		//body part highlights it adds next
		if (g_scriptHighlightsOwned)
		{
			ResetHighlightDataForVATS(vatsData);
			g_scriptHighlightsOwned = false;
		}
		g_deferredHighlight = false;
		VATSHighlightData_SetMode(vatsData, mode);
	}

	void __fastcall Hook_SetupImageSpace(void* vatsData, void* edx, void* sceneTexture, void* vatsTexture, UInt32 scanTexture, bool abIsRenderedMenu)
	{
		//target and viewport here are not the scene's, so fall back to vanilla placement
		if (g_deferredHighlight)
		{
			g_deferredHighlight = false;
			if (HasAdditionalRefs(vatsData))
				RenderHighlightVanilla(vatsData, vatsTexture);
		}

		SetupImageSpace(vatsData, sceneTexture, vatsTexture, scanTexture, abIsRenderedMenu);
	}

	void ClearState()
	{
		g_deferredHighlight = false;
		g_scriptHighlightsOwned = false;
		g_forceSceneDepthTest = false;
	}

	void Init()
	{
		WriteRelCallOrLog(s_handleVATSOcclusionQueriesCall, 0x870C63, reinterpret_cast<UInt32>(Hook_TESMain_HandleVATSOcclusionQueries), 0x874AC0, "TESMain::HandleVATSOcclusionQueries");
		WriteRelCallOrLog(s_renderScenePostResolveDepthCall, 0x8741DB, reinterpret_cast<UInt32>(Hook_RenderScenePostResolveDepth), 0xB6B930, "BSShaderUtil::RenderScenePostResolveDepth");
		WriteRelCallOrLog(s_setupImageSpaceCall, 0x8760A9, reinterpret_cast<UInt32>(Hook_SetupImageSpace), 0x801A60, "VATSHighlightData::SetupImageSpace");
		WriteRelCallOrLog(s_setZEnableCall, 0xBC9C92, reinterpret_cast<UInt32>(Hook_SetZEnable), 0xB97DE0, "BSRenderState::SetZEnable");
		WriteRelCallOrLog(s_setAdditionalRefModeCall, 0x7F3E5D, reinterpret_cast<UInt32>(Hook_VATSMenu_SetAdditionalRefMode), 0x44AC20, "VATSMenu additional-ref mode set");
		WriteScriptCommandJumpOrLog(s_highlightAdditionalReferenceDetour, 0x5BB610, reinterpret_cast<UInt32>(Hook_HighlightAdditionalReference), s_highlightAdditionalReference, "Script::HighlightAdditionalReference");
		WriteScriptCommandJumpOrLog(s_deactivateAllHighlightsDetour, 0x5BB6C0, reinterpret_cast<UInt32>(Hook_DeactivateAllHighlights), s_deactivateAllHighlights, "Cmd_DeactivateAllHighlights");
	}
}
