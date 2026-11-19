#include "SmallDebrisCapEngine.h"

#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/GameLayout.h"
#include "internal/HavokLayout.h"
#include "internal/NiLayout.h"

namespace SmallDebrisCap::Engine {

	void Retain(void* node) { ThisCall(0x40F6E0, node); }

	void Release(void* node) { ThisCall(0x401970, node); }

	void* Parent(void* node) { return NiAVObjectAsView(node)->parent; }

	bool IsFadeNode(void* node)
	{
		return ThisCall<void*>((*static_cast<UInt32**>(node))[4], node) != nullptr;
	}

	void* Root(void* node)
	{
		for (UInt32 depth = 0; node && depth < 16; ++depth, node = Parent(node))
			if (IsFadeNode(node)) return node;
		return nullptr;
	}

	bool InScene(void* node)
	{
		void* root = Root(node);
		return Parent(root ? root : node) != nullptr;
	}

	void* DebrisNode(void* body)
	{
		constexpr UInt32 kLayerMask = 0x7F;
		constexpr UInt32 kSmallDebrisLayer = 19;
		if (!body || (static_cast<HkpWorldObjectView*>(body)->collisionFilterInfo & kLayerMask) != kSmallDebrisLayer)
			return nullptr;
		return CdeclCall<void*>(0xC7FA90, static_cast<UInt8*>(body) + 0x10);
	}

	TESObjectREFR* CullableRef(void* node)
	{
		auto* ref = CdeclCall<TESObjectREFR*>(0x56F930, node);
		if (!ref || ref->typeID != kFormType_Reference || TESObjectREFRGetNiNodeRaw(ref) != node)
			return nullptr;
		if (ref->IsDeleted() || ref->IsPersistent() || ThisCall<void*>(0x56A9F0, ref))
			return nullptr;
		return ref;
	}

	void Fade(void* object, float fraction)
	{
		void* geometry = ThisCall<void*>((*static_cast<UInt32**>(object))[6], object);
		if (geometry) {
			auto* shader = static_cast<NiGeometryFadeView*>(geometry)->shader;
			if (shader && shader->propertyIndex != -1) shader->fade = fraction;
		}
		//0xB6BB30 walks NiNode children and cannot take a geometry leaf
		if (void* node = NiAVObjectGetAsNiNode(object))
			CdeclCall(0xB6BB30, node, fraction);
	}

	void Cull(void* node, bool whole, const bool& cancelled)
	{
		if (whole) {
			if (auto* ref = CullableRef(node)) {
				const UInt32 refID = ref->refID;
				ThisCall<void>(0x574400, ref);
				//disable can delete the ref through another plugin's callbacks
				if (!cancelled && (refID >> 24) == 0xFF) {
					auto* live = static_cast<TESForm*>(::Engine::LookupFormByID(refID));
					if (live == ref && live->typeID == kFormType_Reference)
						CdeclCall<bool>(0x5D8E20, nullptr, nullptr, live);
				}
			}
		} else if (void* parent = Parent(node)) {
			void* detached = nullptr;
			//detachchild returns an owning pointer through its second argument
			ThisCall<void>((*static_cast<UInt32**>(parent))[0x39], parent, node, &detached);
			if (detached) Release(detached);
		}
	}

	SInt32 MaxCount()
	{
		return *static_cast<SInt32*>(::Engine::GetSettingDataPtr(reinterpret_cast<void*>(0x11CF3E4)));
	}

	float FadeTime()
	{
		return ::Engine::GetSettingFloatValue(reinterpret_cast<void*>(0x11CFBFC), 1.0f);
	}
}
