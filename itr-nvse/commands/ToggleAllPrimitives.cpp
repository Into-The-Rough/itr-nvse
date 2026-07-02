//ToggleAllPrimitives (TAP) - show both primitive refs and regular marker refs
//fixes the vanilla load-time cull paths so newly loaded refs honor current visibility

#include "ToggleAllPrimitives.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "internal/CallTemplates.h"
#include "internal/SafeWrite.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/globals.h"
#include "internal/GameGlobals.h"

#include <cstdarg>
#include <cmath>
#include <unordered_map>
#include <vector>

extern void Log(const char* fmt, ...);

namespace
{
	struct GridCellArrayView
	{
		void* vtbl;
		UInt8 pad04[0x0C - 0x04];
		UInt32 gridSize;
		TESObjectCELL** gridCells;
	};
	static_assert(offsetof(GridCellArrayView, gridSize) == 0x0C);
	static_assert(offsetof(GridCellArrayView, gridCells) == 0x10);

	struct TESView
	{
		void* vtbl;
		UInt32 unk04;
		GridCellArrayView* gridCellArray;
		UInt8 pad0C[0x34 - 0x0C];
		TESObjectCELL* currentInterior;
		TESObjectCELL** interiorsBuffer;
		TESObjectCELL** exteriorsBuffer;
		UInt32 interiorBufferSize;
		UInt32 exteriorBufferSize;
	};
	static_assert(offsetof(TESView, gridCellArray) == 0x08);
	static_assert(offsetof(TESView, currentInterior) == 0x34);
	static_assert(offsetof(TESView, interiorsBuffer) == 0x38);
	static_assert(offsetof(TESView, exteriorsBuffer) == 0x3C);
	static_assert(offsetof(TESView, interiorBufferSize) == 0x40);
	static_assert(offsetof(TESView, exteriorBufferSize) == 0x44);

	constexpr UInt32 kAddr_TES_ShowCellNode = 0x456D00;
	constexpr UInt32 kAddr_TES_TogglePrimitive = 0x456AA0;
	constexpr UInt32 kAddr_ToggleCellNodesFlags_GetFlag = 0x5468F0;
	constexpr UInt32 kAddr_INISettingCollection_GetValueAddr = 0x408D60;
	constexpr UInt32 kAddr_ExtraDataList_GetPrimitive = 0x41FBE0;
	constexpr UInt32 kAddr_TESObjectREFR_TogglePrimitive = 0x577390;
	constexpr UInt32 kAddr_TESObjectREFR_IsMarker = 0x444ED0;
	constexpr UInt32 kAddr_TESObject_IsMarker = 0x50F9E0;
	constexpr UInt32 kAddr_TES_AddTempNode = 0x458E20;
	constexpr UInt32 kAddr_NiAVObject_SetLocalTranslate = 0x440460;
	constexpr UInt32 kAddr_NiAVObject_SetLocalScale = 0x440490;
	constexpr UInt32 kAddr_NiAVObject_SetAppCulled = 0x450F90;
	constexpr UInt32 kAddr_NiNode_AttachChild = 0xA5ED10;
	constexpr UInt32 kAddr_BSShaderUtil_AddNoLightingPropertyRecurse = 0x4B6E90;
	constexpr UInt32 kAddr_TES_DrawAxesLines = 0xC59200;
	constexpr UInt32 kAddr_TES_DrawArrow = 0xC59A50;
	constexpr UInt32 kAddr_TES_DrawSphere = 0xC56680;
	constexpr UInt32 kAddr_Load3D_PrimitiveCullCall = 0x56B4DC;
	constexpr UInt32 kAddr_Load3D_DoorTravelCullCall = 0x56BAC8;
	Detours::CallDetour s_load3DPrimitiveCullCall;
	Detours::CallDetour s_load3DDoorTravelCullCall;
	constexpr UInt32 kAddr_NiAlphaProperty_Create = 0xA5CEB0;
	constexpr UInt32 kAddr_NiObject_Alloc = 0xAA13E0;
	constexpr UInt32 kAddr_BSShaderNoLightingProperty_Create = 0xB6FC90;
	constexpr UInt32 kAddr_NiGeometry_AddPropertyNode = 0x43A010;
	constexpr UInt32 kAddr_NiAVObject_AssignGeometryPropsTail = 0xB57BD0;
	constexpr UInt32 kAddr_BSFadeNode_SetAlphaRecurse = 0xB6BB30;
	constexpr UInt32 kAddr_NiAVObject_UpdateProperties = 0xA5A040;
	constexpr UInt32 kAddr_NiNode_UpdateDownwardPass = 0xA5DD70;
	constexpr UInt32 kAddr_ReturnThis = 0x6815C0;
	constexpr UInt32 kAddr_ReturnThis2 = 0xE68810;

	constexpr UInt32 kNiFlag_AppCulled = 0x00000001;
	constexpr UInt32 kNiFlag_AlwaysDraw = 0x00000800;
	constexpr UInt32 kNiFlag_IgnoreFade = 0x00008000;
	constexpr UInt32 kRefreshIntervalMs = 1000;
	constexpr float kOverlayLifetimeSeconds = 3.0f;
	constexpr float kMaxOverlayDistance = 4096.0f;
	enum CellNode : UInt32
	{
		kCellNode_Actors = 0,
		kCellNode_Markers = 1,
		kCellNode_Land = 2,
		kCellNode_Static = 3,
		kCellNode_Dynamic = 4,
		kCellNode_OcclusionPlanes = 5,
		kCellNode_Portals = 6,
		kCellNode_Multibounds = 7,
		kCellNode_Collision = 8,
	};

	constexpr UInt32 kVisibleNodeCount = 5;
	constexpr UInt32 kVisibleNodes[kVisibleNodeCount] = {
		kCellNode_Markers,
		kCellNode_OcclusionPlanes,
		kCellNode_Portals,
		kCellNode_Multibounds,
		kCellNode_Collision,
	};

	struct RefreshCounts
	{
		UInt32 cells = 0;
		UInt32 primitiveRefs = 0;
		UInt32 primitiveRefs3D = 0;
		UInt32 markerRefs = 0;
		UInt32 markerRefs3D = 0;
		UInt32 overlayCandidates = 0;
		UInt32 overlayRefs = 0;
		UInt32 overlayOutOfRange = 0;
		UInt32 overlayNo3D = 0;
		UInt32 overlayStateMisses = 0;
		UInt32 overlayThrottled = 0;
		UInt32 overlayCreateFails = 0;
		float nearestRefDistance = -1.0f;
	};

	enum class OverlaySpawnResult
	{
		kSpawned = 0,
		kNo3D,
		kStateMiss,
		kThrottled,
		kCreateFailed,
	};

	struct NiPoint3
	{
		float x;
		float y;
		float z;
	};

	struct NiColorAlpha
	{
		float r;
		float g;
		float b;
		float a;
	};

	struct NiAlphaPropertyView
	{
		UInt8 pad00[0x18];
		UInt16 flags;
	};
	static_assert(offsetof(NiAlphaPropertyView, flags) == 0x18);

	struct NiAVObjectView
	{
		UInt8 pad00[0x30];
		UInt32 flags;
		UInt8 pad34[0x64 - 0x34];
		float localScale;
	};
	static_assert(offsetof(NiAVObjectView, flags) == 0x30);
	static_assert(offsetof(NiAVObjectView, localScale) == 0x64);

	struct NiUpdateData
	{
		float timePassed;
		bool updateControllers;
		bool isMultiThreaded;
		UInt8 byte06;
		bool updateGeomorphs;
		bool updateShadowScene;
		UInt8 pad09[3];
	};
	static_assert(sizeof(NiUpdateData) == 0x0C);

	struct DebugNodeState
	{
		void* node = nullptr;
		UInt32 flags = 0;
		float localScale = 1.0f;
		UInt32 lastOverlayTick = 0;
	};

	static std::unordered_map<UInt32, DebugNodeState> s_debugNodeStates;
	static bool s_enabled = false;
	static UInt32 s_lastRefreshMs = 0;
	static void* s_alphaProperty = nullptr;

	TESView* GetTESView()
	{
		return static_cast<TESView*>(::GetTES());
	}

	TESObjectREFR* GetPlayer()
	{
		return *g_thePlayerPtr;
	}

	UInt8* GetPrimitivesSetting()
	{
		return Engine::GetSettingUInt8Ptr(GetPrimitivesEnabledSetting());
	}

	bool ArePrimitivesVisible()
	{
		if (UInt8* value = GetPrimitivesSetting())
			return *value != 0;
		return false;
	}

	bool IsCellNodeVisible(UInt32 nodeIndex)
	{
		return !CdeclCall<bool>(kAddr_ToggleCellNodesFlags_GetFlag, nodeIndex);
	}

	bool AreVisibleNodesEnabled()
	{
		for (UInt32 nodeIndex : kVisibleNodes)
		{
			if (!IsCellNodeVisible(nodeIndex))
				return false;
		}
		return true;
	}

	void SetVisibleNodesEnabled(TESView* tes, bool show)
	{
		if (!tes)
			return;

		for (UInt32 nodeIndex : kVisibleNodes)
		{
			if (IsCellNodeVisible(nodeIndex) != show)
				ThisCall<void>(kAddr_TES_ShowCellNode, tes, nodeIndex, show ? 1u : 0u, 0u);
		}
	}

	void SetPrimitivesVisible(TESView* tes, bool show)
	{
		if (!tes || ArePrimitivesVisible() == show)
			return;

		ThisCall<void>(kAddr_TES_TogglePrimitive, tes);
	}

	bool HasPrimitive(TESObjectREFR* refr)
	{
		return refr && ThisCall<void*>(kAddr_ExtraDataList_GetPrimitive, &refr->extraDataList) != nullptr;
	}

	bool IsMarkerRef(TESObjectREFR* refr)
	{
		if (!refr)
			return false;

		if (ThisCall<bool>(kAddr_TESObjectREFR_IsMarker, refr))
			return true;

		return refr->baseForm && ThisCall<bool>(kAddr_TESObject_IsMarker, refr->baseForm);
	}

	void SetRefCulled(TESObjectREFR* refr, bool culled)
	{
		if (!refr || !refr->renderState || !refr->renderState->niNode)
			return;

		ThisCall<void>(kAddr_NiAVObject_SetAppCulled, refr->renderState->niNode, culled ? 1u : 0u);
	}

	void UpdateNode(void* node)
	{
		if (!node)
			return;

		ThisCall<void>(kAddr_NiAVObject_UpdateProperties, node);

		NiUpdateData updateData = {};
		ThisCall<void>(kAddr_NiNode_UpdateDownwardPass, node, &updateData, 0);
	}

	float GetDistance(TESObjectREFR* a, TESObjectREFR* b)
	{
		if (!a || !b)
			return -1.0f;

		const float dx = a->posX - b->posX;
		const float dy = a->posY - b->posY;
		const float dz = a->posZ - b->posZ;
		return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
	}

	bool IsWithinOverlayRange(TESObjectREFR* refr, RefreshCounts& counts)
	{
		TESObjectREFR* player = GetPlayer();
		if (!player || !refr)
			return true;

		const float distance = GetDistance(player, refr);
		if (distance < 0.0f)
			return true;

		if (counts.nearestRefDistance < 0.0f || distance < counts.nearestRefDistance)
			counts.nearestRefDistance = distance;

		return distance <= kMaxOverlayDistance;
	}

	NiColorAlpha GetOverlayColor(bool hasPrimitive, bool isMarker)
	{
		if (hasPrimitive && isMarker)
			return { 1.0f, 0.95f, 0.15f, 1.0f };
		if (hasPrimitive)
			return { 1.0f, 0.35f, 0.05f, 1.0f };
		return { 0.10f, 0.95f, 1.0f, 1.0f };
	}

	float GetOverlayDiameter(bool, bool)
	{
		return 8.0f;
	}

	float GetBeaconBaseAxesSize(bool hasPrimitive, bool isMarker)
	{
		if (hasPrimitive && isMarker)
			return 96.0f;
		if (hasPrimitive)
			return 112.0f;
		if (isMarker)
			return 80.0f;
		return 64.0f;
	}

	float GetBeaconHeight(bool hasPrimitive, bool isMarker)
	{
		if (hasPrimitive && isMarker)
			return 900.0f;
		if (hasPrimitive)
			return 1100.0f;
		if (isMarker)
			return 720.0f;
		return 640.0f;
	}

	float GetBeaconHeadDiameter(bool hasPrimitive, bool isMarker)
	{
		if (hasPrimitive && isMarker)
			return 112.0f;
		if (hasPrimitive)
			return 128.0f;
		if (isMarker)
			return 96.0f;
		return 80.0f;
	}

	float GetOverlayOffsetZ(bool hasPrimitive, bool isMarker)
	{
		if (hasPrimitive)
			return 96.0f;
		if (isMarker)
			return 64.0f;
		return 48.0f;
	}

	void ApplyDebugNodeFlags(void* node, bool addNoLighting = true)
	{
		if (!node)
			return;

		auto* view = reinterpret_cast<NiAVObjectView*>(node);
		view->flags &= ~kNiFlag_AppCulled;
		view->flags |= kNiFlag_AlwaysDraw | kNiFlag_IgnoreFade;
		ThisCall<void>(kAddr_NiAVObject_SetAppCulled, node, 0u);
		CdeclCall<void>(kAddr_BSFadeNode_SetAlphaRecurse, node, 1.0f);
		if (addNoLighting)
			CdeclCall<void>(kAddr_BSShaderUtil_AddNoLightingPropertyRecurse, node);
	}

	__declspec(naked) void __fastcall AddPropertyToGeometry(void* geometry, void* property)
	{
		__asm
		{
			lock inc dword ptr [edx+4]
			push	ecx
			push	edx
			call	kAddr_NiGeometry_AddPropertyNode
			pop		dword ptr [eax+8]
			pop		ecx
			inc		dword ptr [ecx+0x2C]
			mov		edx, [ecx+0x24]
			mov		[ecx+0x24], eax
			test	edx, edx
			jz		emptyList
			mov		[eax], edx
			mov		[edx+4], eax
			retn
		emptyList:
			mov		[ecx+0x28], eax
			retn
		}
	}

	__declspec(naked) void __fastcall AssignGeometryProps(void* object)
	{
		__asm
		{
			push	0
			push	0
			push	0
			push	ecx
			call	kAddr_NiAVObject_UpdateProperties
			call	kAddr_NiAVObject_AssignGeometryPropsTail
			add		esp, 0x10
			retn
		}
	}

	bool IsNodeObject(void* object)
	{
		return object && (*reinterpret_cast<UInt32**>(object))[0x0C >> 2] == kAddr_ReturnThis;
	}

	bool IsGeometryObject(void* object)
	{
		return object && (*reinterpret_cast<UInt32**>(object))[0x18 >> 2] == kAddr_ReturnThis2;
	}

	struct NiNodeChildrenView
	{
		UInt8 pad00[0x9C];
		NiTArray<void*> children;
	};
	static_assert(offsetof(NiNodeChildrenView, children) == 0x9C);

	void* GetAlphaProperty()
	{
		if (!s_alphaProperty)
		{
			s_alphaProperty = CdeclCall<void*>(kAddr_NiAlphaProperty_Create);
			if (s_alphaProperty)
				reinterpret_cast<NiAlphaPropertyView*>(s_alphaProperty)->flags = 0x10ED;
		}

		return s_alphaProperty;
	}

	void* CreateNoLightingProperty()
	{
		void* memory = CdeclCall<void*>(kAddr_NiObject_Alloc, 0x80u);
		return memory ? ThisCall<void*>(kAddr_BSShaderNoLightingProperty_Create, memory) : nullptr;
	}

	void ApplyRenderPropertiesRecursive(void* object, void* alphaProperty)
	{
		if (!object)
			return;

		if (IsNodeObject(object))
		{
			auto* node = reinterpret_cast<NiNodeChildrenView*>(object);
			for (UInt16 i = 0; i < node->children.firstFreeEntry; i++)
				ApplyRenderPropertiesRecursive(node->children.data ? node->children.data[i] : nullptr, alphaProperty);
			return;
		}

		if (!IsGeometryObject(object))
			return;

		if (void* noLighting = CreateNoLightingProperty())
			AddPropertyToGeometry(object, noLighting);

		if (alphaProperty)
			AddPropertyToGeometry(object, alphaProperty);
	}

	void FinalizeOverlayVisuals(void* overlay)
	{
		void* alphaProperty = GetAlphaProperty();
		ApplyRenderPropertiesRecursive(overlay, alphaProperty);
		AssignGeometryProps(overlay);
	}

	void* CreateDebugOverlay(bool hasPrimitive, bool isMarker)
	{
		const float diameter = GetOverlayDiameter(hasPrimitive, isMarker);
		const float baseAxesSize = GetBeaconBaseAxesSize(hasPrimitive, isMarker);
		const float beaconHeight = GetBeaconHeight(hasPrimitive, isMarker);
		const float headDiameter = GetBeaconHeadDiameter(hasPrimitive, isMarker);
		const NiColorAlpha color = GetOverlayColor(hasPrimitive, isMarker);
		void* overlay = CdeclCall<void*>(kAddr_TES_DrawSphere, diameter, 10u, 10u, &color);
		if (!overlay)
			return nullptr;

		if (void* axes = CdeclCall<void*>(kAddr_TES_DrawAxesLines, baseAxesSize))
			ThisCall<void>(kAddr_NiNode_AttachChild, overlay, axes, 1u);

		const NiPoint3 arrowVector = { 0.0f, 0.0f, beaconHeight };
		if (void* arrow = CdeclCall<void*>(kAddr_TES_DrawArrow, &arrowVector, &color))
			ThisCall<void>(kAddr_NiNode_AttachChild, overlay, arrow, 1u);

		const NiPoint3 topPos = { 0.0f, 0.0f, beaconHeight };
		if (void* head = CdeclCall<void*>(kAddr_TES_DrawSphere, headDiameter, 10u, 10u, &color))
		{
			ThisCall<void>(kAddr_NiAVObject_SetLocalTranslate, head, &topPos);
			ThisCall<void>(kAddr_NiNode_AttachChild, overlay, head, 1u);
		}

		if (void* topAxes = CdeclCall<void*>(kAddr_TES_DrawAxesLines, headDiameter * 1.4f))
		{
			ThisCall<void>(kAddr_NiAVObject_SetLocalTranslate, topAxes, &topPos);
			ThisCall<void>(kAddr_NiNode_AttachChild, overlay, topAxes, 1u);
		}

		FinalizeOverlayVisuals(overlay);
		ApplyDebugNodeFlags(overlay);
		UpdateNode(overlay);
		return overlay;
	}

	void ApplyLoudVisuals(TESObjectREFR* refr, bool hasPrimitive, bool isMarker)
	{
		if (!refr || !refr->renderState || !refr->renderState->niNode)
			return;

		void* node = refr->renderState->niNode;
		NiAVObjectView* view = reinterpret_cast<NiAVObjectView*>(node);
		DebugNodeState& state = s_debugNodeStates[refr->refID];
		const bool newNode = state.node != node;
		if (newNode)
		{
			state.node = node;
			state.flags = view->flags;
			state.localScale = view->localScale;
			state.lastOverlayTick = 0;
		}

		ApplyDebugNodeFlags(node, false);

		float scale = state.localScale;
		if (isMarker && !hasPrimitive)
			scale *= 4.0f;
		else if (hasPrimitive)
			scale *= 1.5f;

		ThisCall<void>(kAddr_NiAVObject_SetLocalScale, node, scale);
		UpdateNode(node);
	}

	OverlaySpawnResult SpawnDebugOverlay(TESView* tes, TESObjectREFR* refr, bool hasPrimitive, bool isMarker)
	{
		if (!tes || !refr || !refr->renderState || !refr->renderState->niNode)
			return OverlaySpawnResult::kNo3D;

		auto iter = s_debugNodeStates.find(refr->refID);
		if (iter == s_debugNodeStates.end())
			return OverlaySpawnResult::kStateMiss;

		DebugNodeState& state = iter->second;
		if (state.node != refr->renderState->niNode)
			return OverlaySpawnResult::kStateMiss;

		const UInt32 now = GetTickCount();
		if (state.lastOverlayTick && now - state.lastOverlayTick < kRefreshIntervalMs)
			return OverlaySpawnResult::kThrottled;

		void* overlay = CreateDebugOverlay(hasPrimitive, isMarker);
		if (!overlay)
			return OverlaySpawnResult::kCreateFailed;

		const NiPoint3 worldPos = { refr->posX, refr->posY, refr->posZ + GetOverlayOffsetZ(hasPrimitive, isMarker) };
		ThisCall<void>(kAddr_NiAVObject_SetLocalTranslate, overlay, &worldPos);
		UpdateNode(overlay);
		ThisCall<void>(kAddr_TES_AddTempNode, tes, overlay, kOverlayLifetimeSeconds);
		state.lastOverlayTick = now;
		return OverlaySpawnResult::kSpawned;
	}

	void AccumulateOverlayResult(RefreshCounts& counts, OverlaySpawnResult result)
	{
		switch (result)
		{
			case OverlaySpawnResult::kSpawned:
				counts.overlayRefs++;
				break;
			case OverlaySpawnResult::kNo3D:
				counts.overlayNo3D++;
				break;
			case OverlaySpawnResult::kStateMiss:
				counts.overlayStateMisses++;
				break;
			case OverlaySpawnResult::kThrottled:
				counts.overlayThrottled++;
				break;
			case OverlaySpawnResult::kCreateFailed:
				counts.overlayCreateFails++;
				break;
		}
	}

	void RestoreVisuals(TESObjectREFR* refr)
	{
		if (!refr)
			return;

		auto iter = s_debugNodeStates.find(refr->refID);
		if (iter == s_debugNodeStates.end())
			return;

		if (refr->renderState && refr->renderState->niNode && refr->renderState->niNode == iter->second.node)
		{
			void* node = refr->renderState->niNode;
			NiAVObjectView* view = reinterpret_cast<NiAVObjectView*>(node);
			view->flags = iter->second.flags;
			ThisCall<void>(kAddr_NiAVObject_SetLocalScale, node, iter->second.localScale);
			UpdateNode(node);
		}

		s_debugNodeStates.erase(iter);
	}

	void RefreshCellRefs(TESView* tes, TESObjectCELL* cell, bool show, RefreshCounts& counts)
	{
		if (!cell)
			return;

		counts.cells++;

		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* refr = iter.Get();
			if (!refr)
				continue;

			const bool hasPrimitive = HasPrimitive(refr);
			const bool isMarker = IsMarkerRef(refr);

			if (!show)
				RestoreVisuals(refr);

			if (hasPrimitive)
			{
				counts.primitiveRefs++;
				if (refr->renderState && refr->renderState->niNode)
					counts.primitiveRefs3D++;
				ThisCall<void>(kAddr_TESObjectREFR_TogglePrimitive, refr, show ? 1u : 0u);
			}

			if (isMarker)
			{
				counts.markerRefs++;
				if (refr->renderState && refr->renderState->niNode)
					counts.markerRefs3D++;
				SetRefCulled(refr, !show);
			}

			if (show && (hasPrimitive || isMarker))
			{
				ApplyLoudVisuals(refr, hasPrimitive, isMarker);
				if (IsWithinOverlayRange(refr, counts))
				{
					counts.overlayCandidates++;
					AccumulateOverlayResult(counts, SpawnDebugOverlay(tes, refr, hasPrimitive, isMarker));
				}
				else
				{
					counts.overlayOutOfRange++;
				}
			}
		}
	}

	void AddUniqueCell(std::vector<TESObjectCELL*>& cells, TESObjectCELL* cell)
	{
		if (!cell)
			return;

		for (TESObjectCELL* existing : cells)
		{
			if (existing == cell)
				return;
		}

		cells.push_back(cell);
	}

	void CollectLoadedCells(TESView* tes, std::vector<TESObjectCELL*>& cells)
	{
		if (!tes)
			return;

		AddUniqueCell(cells, tes->currentInterior);

		if (tes->gridCellArray && tes->gridCellArray->gridCells)
		{
			const UInt32 cellCount = tes->gridCellArray->gridSize * tes->gridCellArray->gridSize;
			for (UInt32 i = 0; i < cellCount; i++)
				AddUniqueCell(cells, tes->gridCellArray->gridCells[i]);
		}

		if (tes->interiorsBuffer)
		{
			for (UInt32 i = 0; i < tes->interiorBufferSize; i++)
				AddUniqueCell(cells, tes->interiorsBuffer[i]);
		}

		if (tes->exteriorsBuffer)
		{
			for (UInt32 i = 0; i < tes->exteriorBufferSize; i++)
				AddUniqueCell(cells, tes->exteriorsBuffer[i]);
		}
	}

	RefreshCounts RefreshLoadedRefs(TESView* tes, bool show)
	{
		RefreshCounts counts;
		std::vector<TESObjectCELL*> cells;
		cells.reserve(32);
		CollectLoadedCells(tes, cells);

		for (TESObjectCELL* cell : cells)
			RefreshCellRefs(tes, cell, show, counts);

		return counts;
	}

	RefreshCounts RefreshEnabledState(TESView* tes)
	{
		SetVisibleNodesEnabled(tes, true);
		SetPrimitivesVisible(tes, true);
		return RefreshLoadedRefs(tes, true);
	}

	void __fastcall Load3D_PrimitiveCullHook(void* node, void*, UInt32 shouldCull)
	{
		if (node && ArePrimitivesVisible())
			shouldCull = 0;

		if (node)
			ThisCall<void>(kAddr_NiAVObject_SetAppCulled, node, shouldCull);
	}

	void __fastcall Load3D_DoorTravelCullHook(void* node, void*, UInt32 shouldCull)
	{
		if (node && AreVisibleNodesEnabled())
			shouldCull = 0;

		if (node)
			ThisCall<void>(kAddr_NiAVObject_SetAppCulled, node, shouldCull);
	}

	bool InstallLoad3DCall(Detours::CallDetour& detour, UInt32 addr, void* hook)
	{
		if (*reinterpret_cast<UInt8*>(addr) != 0xE8)
		{
			Log("ToggleAllPrimitives: site 0x%08X expected CALL, found 0x%02X",
				addr, *reinterpret_cast<UInt8*>(addr));
			return false;
		}
		return detour.WriteRelCall(addr, hook);
	}

	void InstallLoad3DFixes()
	{
		static bool s_attempted = false;
		if (s_attempted)
			return;
		s_attempted = true;

		InstallLoad3DCall(s_load3DPrimitiveCullCall, kAddr_Load3D_PrimitiveCullCall, Load3D_PrimitiveCullHook);
		InstallLoad3DCall(s_load3DDoorTravelCullCall, kAddr_Load3D_DoorTravelCullCall, Load3D_DoorTravelCullHook);
	}
}

DEFINE_COMMAND_ALT_PLUGIN(ToggleAllPrimitives, TAP, "Toggle all primitives and marker refs with fixed load-time behaviour", 0, 0, nullptr)

bool Cmd_ToggleAllPrimitives_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESView* tes = GetTESView();
	if (!tes)
		return true;

	const bool enable = !s_enabled;
	RefreshCounts counts;

	if (enable)
	{
		s_enabled = true;
		s_lastRefreshMs = 0;
		counts = RefreshEnabledState(tes);
	}
	else
	{
		s_enabled = false;
		SetVisibleNodesEnabled(tes, false);
		SetPrimitivesVisible(tes, false);
		counts = RefreshLoadedRefs(tes, false);
		s_debugNodeStates.clear();
	}

	*result = enable ? 1.0 : 0.0;

	if (IsConsoleMode())
	{
		Console_Print("ToggleAllPrimitives >> %s primitives=%u markers=%u overlays=%u",
			enable ? "enabled" : "disabled",
			counts.primitiveRefs, counts.markerRefs, counts.overlayRefs);
	}

	return true;
}

namespace ToggleAllPrimitives
{
	void Update()
	{
		if (!s_enabled)
			return;

		const UInt32 now = GetTickCount();
		if (s_lastRefreshMs && now - s_lastRefreshMs < kRefreshIntervalMs)
			return;

		s_lastRefreshMs = now;

		if (TESView* tes = GetTESView())
		{
			for (auto it = s_debugNodeStates.begin(); it != s_debugNodeStates.end();)
			{
				if (!Engine::LookupFormByID(it->first)) it = s_debugNodeStates.erase(it);
				else ++it;
			}
			RefreshEnabledState(tes);
		}
	}

	void Reset()
	{
		s_enabled = false;
		s_debugNodeStates.clear();
		s_lastRefreshMs = 0;
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_ToggleAllPrimitives);
	}

	//code patches, runtime only - registration runs in the GECK too where these addresses differ
	void InstallHooks()
	{
		InstallLoad3DFixes();
	}
}
